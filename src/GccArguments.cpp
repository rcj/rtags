#include "GccArguments.h"
#include "gccopts_gperf.h"
#include "Log.h"
#include "RTags.h"

class GccArgumentsImpl
{
public:
    GccArgumentsImpl() : type(GccArguments::NoType), lang(GccArguments::NoLang) { }

    List<ByteArray> clangArgs;
    List<Path> inputFiles;
    List<Path> includes;
    Path outputFile;
    GccArguments::Type type;
    GccArguments::Lang lang;
    Path base, compiler;
};

GccArguments::GccArguments()
    : mImpl(new GccArgumentsImpl)
{
}

GccArguments::GccArguments(const ByteArray &args, const Path &base)
    : mImpl(new GccArgumentsImpl)
{
    parse(args, base);
}

GccArguments::~GccArguments()
{
    delete mImpl;
}

void GccArguments::clear()
{
    *mImpl = GccArgumentsImpl();
}

static inline GccArguments::Lang guessLang(const Path &fullPath)
{
    ByteArray compiler = fullPath.fileName();
    ByteArray c;
    int dash = compiler.lastIndexOf('-');
    if (dash >= 0) {
        c = ByteArray(compiler.constData() + dash + 1, compiler.size() - dash - 1);
    } else {
        c = ByteArray(compiler.constData(), compiler.size());
    }

    if (c.size() != compiler.size()) {
        bool isVersion = true;
        for (int i=0; i<c.size(); ++i) {
            if ((c.at(i) < '0' || c.at(i) > '9') && c.at(i) != '.') {
                isVersion = false;
                break;
            }
        }
        if (isVersion) {
            dash = compiler.lastIndexOf('-', dash - 1);
            if (dash >= 0) {
                c = compiler.mid(dash + 1, compiler.size() - c.size() - 2 - dash);
            } else {
                c = compiler.left(dash);
            }
        }
    }

    GccArguments::Lang lang = GccArguments::NoLang;
    if (c == "g++" || c == "c++") {
        lang = GccArguments::CPlusPlus;
    } else if (c == "gcc" || c == "cc") {
        lang = GccArguments::C;
    }
    return lang;
}

static inline void eatAutoTools(List<ByteArray> &args)
{
    List<ByteArray> copy = args;
    for (int i=0; i<args.size(); ++i) {
        if (args.at(i).contains("gcc") || args.at(i).contains("g++") || args.at(i) == "cd" || args.at(i) == "c++") {
            if (i) {
                args.erase(args.begin(), args.begin() + i);
                if (testLog(Debug)) {
                    debug() << "ate something " << copy;
                    debug() << "now we have " << args;
                }
            }
            break;
        }
    }
}

static inline ByteArray trim(const char *start, int size)
{
    while (size && isspace(*start)) {
        ++start;
        --size;
    }
    while (size && isspace(start[size - 1])) {
        --size;
    }
    return ByteArray(start, size);
}

bool GccArguments::parse(ByteArray args, const Path &base)
{
    mImpl->type = NoType;
    mImpl->lang = NoLang;
    mImpl->clangArgs.clear();
    mImpl->inputFiles.clear();
    mImpl->base = base;

    char quote = '\0';
    List<ByteArray> split;
    ByteArray old2 = args;
    {
        char *cur = args.data();
        char *prev = cur;
        // ### handle escaped quotes?
        int size = args.size();
        while (size > 0) {
            switch (*cur) {
            case '\\':
                assert(size > 0);
                memmove(cur, cur + 1, size);
                --size;
                break;

            case '"':
            case '\'':
                if (quote == '\0')
                    quote = *cur;
                else if (*cur == quote)
                    quote = '\0';
                memmove(cur, cur + 1, size);
                --size;
                break;
            case ' ':
                if (quote == '\0') {
                    if (cur > prev)
                        split.append(trim(prev, cur - prev));
                    prev = cur + 1;
                }
                break;
            default:
                break;
            }
            --size;
            ++cur;
        }
        if (cur > prev)
            split.append(trim(prev, cur - prev));
    }
    eatAutoTools(split);

    if (split.isEmpty()) {
        clear();
        return false;
    }
    debug() << "GccArguments::parse (" << args << ") => " << split;

    Path path;
    if (split.front() == "cd" && split.size() > 3 && split.at(2) == "&&") {
        path = Path::resolved(split.at(1), base);
        split.erase(split.begin(), split.begin() + 3);
    } else {
        path = base;
    }

    mImpl->lang = guessLang(split.front());
    if (mImpl->lang == NoLang) {
        clear();
        return false;
    }

    List<ByteArray> unresolvedInputs;

    bool pathok = false;
    char prevopt = '\1'; // skip the initial binary name

    gccopts_gperf gccopts;
    const int s = split.size();
    for (int i=0; i<s; ++i) {
        const ByteArray &arg = split.at(i);
        const char *cur = arg.constData();
        if (prevopt != '\0') {
            switch (prevopt) {
            case 'x':
                if (!strcmp(cur, "c-header")) {
                    mImpl->type = Pch;
                    assert(mImpl->lang == C);
                } else if (!strcmp(cur, "c++-header")) {
                    mImpl->type = Pch;
                    assert(mImpl->lang == CPlusPlus);
                }
                mImpl->clangArgs.append("-x");
                mImpl->clangArgs.append(cur);
                break;
            case 'i': {
                Path inc = Path::resolved(cur + ByteArray(".gch"), path, &pathok);
                if (!pathok) // try without .gch postfix
                    inc = Path::resolved(cur, path, &pathok);
                if (pathok) {
                    mImpl->includes.append(inc);
                } else {
                    if (!inc.isAbsolute()) {
                        mImpl->includes.append(Path(path + "/" + cur + ByteArray(".gch"))); // ### is assuming .gch correct here?
                    } else {
                        warning("-include %s could not be resolved", cur);
                    }
                } }
                break;
            case 'o': {
                if (!mImpl->outputFile.isEmpty())
                    warning("Already have an output file: %s (new %s)",
                            mImpl->outputFile.constData(), cur);
                Path out = Path::resolved(cur, path);
                mImpl->outputFile = out; }
                break;
            default:
                break;
            }
            prevopt = '\0';
            continue;
        }
        if (!strncmp(cur, "-", 1)) { // option
            if (gccopts.in_word_set(cur, strlen(cur))) {
                if (!strcmp(cur, "-x"))
                    prevopt = 'x';
                else if (!strcmp(cur, "-o"))
                    prevopt = 'o';
                else if (!strcmp(cur, "-include"))
                    prevopt = 'i';
                else
                    prevopt = '\1';
                continue;
            } else {
                if (!strncmp(cur, "-D", 2)) {
                    ByteArray arg;
                    if (arg.size() == 2 && i + 1 < s) {
                        arg = (cur + split.at(++i));
                    } else {
                        arg = cur;
                    }
                    mImpl->clangArgs.append(arg);
                } else if (!strncmp(cur, "-I", 2)) {
                    Path inc;
                    pathok = false;
                    if (arg.size() > 2) {
                        inc = Path::resolved(cur + 2, path, &pathok);
                    } else if (i + 1 < s) {
                        inc = Path::resolved(split.at(++i), path, &pathok);
                    }
                    if (pathok)
                        mImpl->clangArgs.append("-I" + inc);
                } else if (mImpl->type == NoType && !strcmp(cur, "-c")) {
                    mImpl->type = Compile;
                }
            }
        } else { // input file?
            Path input = Path::resolved(cur, path, &pathok);
            if (pathok)
                mImpl->inputFiles.append(input);
            else
                unresolvedInputs.append(cur);
        }
    }

    if (mImpl->type == NoType) {
        clear();
        return false;
    }

    if (mImpl->inputFiles.isEmpty()) {
        error("Unable to find or resolve input files");
        const int c = unresolvedInputs.size();
        for (int i=0; i<c; ++i) {
            const ByteArray &input = unresolvedInputs.at(i);
            error("  %s", input.constData());
        }
        clear();
        return false;
    }
    if (mImpl->outputFile.isEmpty() && mImpl->type == Pch) {
        error("Output file is empty for pch");
        clear();
        return false;
    }
    if (!mImpl->outputFile.isResolved() && !mImpl->outputFile.isAbsolute()) {
        mImpl->outputFile = path + "/" + mImpl->outputFile;
    }

    mImpl->compiler = split.front();
    if (!mImpl->compiler.isResolved()) {
        static Map<Path, Path> resolvedFromPath;
        static List<Path> paths;
        mImpl->compiler = mImpl->compiler.fileName();
        Path resolved = resolvedFromPath.value(mImpl->compiler);
        if (resolved.isEmpty()) {
            if (paths.isEmpty()) {
                const ByteArray p = getenv("PATH");
                const List<ByteArray> split = p.split(':');
                const int splitCount = split.size();
                for (int i=0; i<splitCount; ++i) {
                    Path path = split.at(i);
                    if (path.resolve()) {
                        if (!path.endsWith('/'))
                            path.append('/');
                        paths.append(path);
                    }
                }
            }
            const char *fncstr = mImpl->compiler.fileName();
            const ByteArray fn(fncstr);
            const int pathCount = paths.size();
            for (int i=0; i<pathCount; ++i) {
                const Path &path = paths.at(i);
                Path c = path + fn;
                if (c.isFile()) {
                    resolvedFromPath[mImpl->compiler] = c;
                    mImpl->compiler = c;
                    break;
                }
            }
        }

    }

    return true;
}

GccArguments::Type GccArguments::type() const
{
    return mImpl->type;
}

GccArguments::Lang GccArguments::lang() const
{
    return mImpl->lang;
}

List<ByteArray> GccArguments::clangArgs() const
{
    return mImpl->clangArgs;
}

List<Path> GccArguments::inputFiles() const
{
    return mImpl->inputFiles;
}

List<ByteArray> GccArguments::explicitIncludes() const
{
    List<ByteArray> incs;
    const int c = mImpl->includes.size();
    for (int i=0; i<c; ++i)
        incs.append(mImpl->includes.at(i));
    // ### ????
    return incs;
}

Path GccArguments::outputFile() const
{
    return mImpl->outputFile;
}

Path GccArguments::baseDirectory() const
{
    return mImpl->base;
}

void GccArguments::addFlags(const List<ByteArray> &extraFlags)
{
    const int count = extraFlags.size();
    for (int i=0; i<count; ++i) {
        ByteArray flag = extraFlags.at(i);
        if (flag.startsWith("-I")) {
            Path p = Path::resolved(flag.constData() + 2);
            flag.replace(2, flag.size() - 2, p);
        }
        mImpl->clangArgs.append(flag);
    }
}

Path GccArguments::compiler() const
{
    return mImpl->compiler;
}
