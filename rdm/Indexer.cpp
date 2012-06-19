#include "Database.h"
#include "Server.h"
#include "DirtyJob.h"
#include "Indexer.h"
#include "IndexerJob.h"
#include "MemoryMonitor.h"
#include "Path.h"
#include "RTags.h"
#include "Rdm.h"
#include "SHA256.h"
#include <Log.h>
#include <QtCore>

Indexer::Indexer(const QByteArray &path, QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<Path>("Path");

    Q_ASSERT(path.startsWith('/'));
    if (!path.startsWith('/'))
        return;

    mJobCounter = 0;
    mPath = path + "pch/";
    Q_ASSERT(mPath.endsWith('/'));
    QDir dir;
    dir.mkpath(mPath);
    mTimerRunning = false;

    connect(&mWatcher, SIGNAL(directoryChanged(QString)),
            this, SLOT(onDirectoryChanged(QString)));

    {
        ScopedDB db = Server::instance()->db(Server::PCHUsrHashes, ScopedDB::Read);
        RTags::Ptr<Iterator> it(db->createIterator());
        it->seekToFirst();
        while (it->isValid()) {
            mPchUSRHashes[it->key().byteArray()] = it->value<PchUSRHash>();
            it->next();
        }
    }
    {
        ScopedDB db = Server::instance()->db(Server::General, ScopedDB::Read);
        mPchDependencies = db->value<QHash<Path, QSet<quint32> > >("pchDependencies");
    }
    {
        // watcher
        ScopedDB db = Server::instance()->db(Server::Dependency, ScopedDB::Read);
        RTags::Ptr<Iterator> it(db->createIterator());
        it->seekToFirst();
        DependencyHash dependencies;
        while (it->isValid()) {
            const Slice key = it->key();
            const quint32 fileId = *reinterpret_cast<const quint32*>(key.data());
            const QSet<quint32> deps = it->value<QSet<quint32> >();
            dependencies[fileId] = deps;
            it->next();
        }
        commitDependencies(dependencies, false);
    }

    initDB();
}

Indexer::~Indexer()
{
    // MutexLocker locker(&mMutex);

    // write out FileInformation for all the files that are waiting for pch maybe
}

static inline bool isFile(quint32 fileId)
{
    return Location::path(fileId).isFile(); // ### not ideal
}

void Indexer::initDB(InitMode mode, const QByteArray &pattern)
{
    Q_ASSERT(mode == ForceDirty || pattern.isEmpty());
    QElapsedTimer timer;
    timer.start();
    QHash<quint32, QSet<quint32> > deps, depsReversed;

    ScopedDB dependencyDB = Server::instance()->db(Server::Dependency, ScopedDB::Read);
    RTags::Ptr<Iterator> it(dependencyDB->createIterator());
    it->seekToFirst();
    {
        Batch batch(dependencyDB);
        while (it->isValid()) {
            const Slice key = it->key();
            const quint32 file = *reinterpret_cast<const quint32*>(key.data());
            if (isFile(file)) {
                const QSet<quint32> v = it->value<QSet<quint32> >();
                depsReversed[file] = v;
                foreach(const quint32 p, v) {
                    deps[p].insert(file);
                }
            } else {
                batch.remove(key);
            }
            it->next();
        }
    }

    QSet<quint32> dirtyFiles;
    QHash<Path, QList<QByteArray> > toIndex, toIndexPch;
    int checked = 0;

    {
        ScopedDB fileInformationDB = Server::instance()->db(Server::FileInformation, ScopedDB::Write);
        Batch batch(fileInformationDB);
        it.reset(fileInformationDB->createIterator());
        it->seekToFirst();
        MutexLocker lock(&mVisitedFilesMutex);
        QRegExp rx(pattern);
        while (it->isValid()) {
            const Slice key = it->key();
            const quint32 fileId = *reinterpret_cast<const quint32*>(key.data());
            const Path path = Location::path(fileId);
            if (path.isFile()) {
                const FileInformation fi = it->value<FileInformation>();
                if (!fi.compileArgs.isEmpty()) {
#ifdef QT_DEBUG
                    if (path.isHeader() && !Rdm::isPch(fi.compileArgs)) {
                        qDebug() << path << fi.compileArgs << fileId;
                        Q_ASSERT(0);
                    }
#endif
                    ++checked;
                    bool dirty = false;
                    const QSet<quint32> dependencies = deps.value(fileId);
                    Q_ASSERT(dependencies.contains(fileId));
                    foreach(quint32 id, dependencies) {
                        if (dirtyFiles.contains(id)) {
                            dirty = true;
                        } else {
                            const Path p = Location::path(id);
                            if ((mode == ForceDirty && (pattern.isEmpty() || QString::fromLocal8Bit(p).contains(rx)))
                                || p.lastModified() > fi.lastTouched) {
                                dirtyFiles.insert(id);
                                dirtyFiles += depsReversed.value(id);
                                dirty = true;
                            }
                        }
                    }
                    if (dirty) {
                        if (Rdm::isPch(fi.compileArgs)) {
                            toIndexPch[path] = fi.compileArgs;
                        } else {
                            toIndex[path] = fi.compileArgs;
                        }
                    } else {
                        mVisitedFiles += dependencies;
                    }
                    warning() << "checking if" << path << "is dirty =>" << dirty;
                }
            } else {
                batch.remove(key);
            }
            it->next();
        }
    }

    if (checked)
        error() << "Checked" << checked << "files. Found" << dirtyFiles.size() << "dirty files and"
                << (toIndex.size() + toIndexPch.size()) << "sources to reindex in" << timer.elapsed() << "ms";

    if (toIndex.isEmpty() && toIndexPch.isEmpty())
        return;

    {
        MutexLocker lock(&mVisitedFilesMutex);
        mVisitedFiles -= dirtyFiles;
    }

    if (dirtyFiles.size() > 1) {
        Server::instance()->threadPool()->start(new DirtyJob(this, dirtyFiles, toIndexPch, toIndex));
    } else {
        for (QHash<Path, QList<QByteArray> >::const_iterator it = toIndexPch.begin(); it != toIndexPch.end(); ++it) {
            index(it.key(), it.value(), IndexerJob::DirtyPch|IndexerJob::NeedsDirty);
        }

        for (QHash<Path, QList<QByteArray> >::const_iterator it = toIndex.begin(); it != toIndex.end(); ++it) {
            index(it.key(), it.value(), IndexerJob::Dirty|IndexerJob::NeedsDirty);
        }
    }
}

void Indexer::commitDependencies(const DependencyHash &deps, bool sync)
{
    DependencyHash newDependencies;

    if (mDependencies.isEmpty()) {
        mDependencies = deps;
        newDependencies = deps;
    } else {
        const DependencyHash::const_iterator end = deps.end();
        for (DependencyHash::const_iterator it = deps.begin(); it != end; ++it) {
            newDependencies[it.key()].unite(it.value() - mDependencies[it.key()]);
            DependencyHash::iterator i = newDependencies.find(it.key());
            if (i.value().isEmpty())
                newDependencies.erase(i);
            mDependencies[it.key()].unite(it.value());
        }
    }
    if (sync && !newDependencies.isEmpty())
        Rdm::writeDependencies(newDependencies);

    Path parentPath;
    QSet<QString> watchPaths;
    const DependencyHash::const_iterator end = newDependencies.end();
    MutexLocker lock(&mWatchedMutex);
    for (DependencyHash::const_iterator it = newDependencies.begin(); it != end; ++it) {
        const Path path = Location::path(it.key());
        parentPath = path.parentDir();
        WatchedHash::iterator wit = mWatched.find(parentPath);
        //debug() << "watching" << path << "in" << parentPath;
        if (wit == mWatched.end()) {
            mWatched[parentPath].insert(qMakePair<QByteArray, time_t>(path.fileName(), path.lastModified()));
            watchPaths.insert(QString::fromLocal8Bit(parentPath));
        } else {
            wit.value().insert(qMakePair<QByteArray, time_t>(path.fileName(), path.lastModified()));
        }
    }
    if (watchPaths.isEmpty())
        return;
    mWatcher.addPaths(watchPaths.toList());
}

int Indexer::index(const Path &input, const QList<QByteArray> &arguments, unsigned indexerJobFlags)
{
    MutexLocker locker(&mMutex);

    if (mIndexing.contains(input))
        return -1;

    const int id = ++mJobCounter;
    IndexerJob *job = new IndexerJob(this, id, indexerJobFlags, input, arguments);
    connect(job, SIGNAL(done(int, Path, bool, QByteArray)),
            this, SLOT(onJobComplete(int, Path, bool, QByteArray)));
    if (needsToWaitForPch(job)) {
        mWaitingForPCH[id] = job;
        return id;
    }
    startJob(id, job);
    return id;
}

void Indexer::startJob(int id, IndexerJob *job)
{
    mJobs[id] = job;
    mIndexing.insert(job->mIn);

    if (!mTimerRunning) {
        mTimerRunning = true;
        mTimer.start();
    }

    Server::instance()->threadPool()->start(job, job->priority());
}

void Indexer::onDirectoryChanged(const QString &path)
{
    const Path p = path.toLocal8Bit();
    QSet<quint32> dirtyFiles;
    QHash<Path, QList<QByteArray> > toIndex, toIndexPch;

    Q_ASSERT(p.endsWith('/'));
    {
        MutexLocker watchedLock(&mWatchedMutex);
        MutexLocker visitedLock(&mVisitedFilesMutex);
        WatchedHash::iterator it = mWatched.find(p);
        if (it == mWatched.end()) {
            error() << "directory changed, but not in watched list" << p;
            return;
        }

        Path file;
        QList<Path> pending;
        QSet<WatchedPair>::iterator wit = it.value().begin();
        QSet<WatchedPair>::const_iterator wend = it.value().end();
        QList<QByteArray> args;

        ScopedDB db = Server::instance()->db(Server::FileInformation, ScopedDB::Read);
        while (wit != wend) {
            // weird API, QSet<>::iterator does not allow for modifications to the referenced value
            file = (p + (*wit).first);
            // qDebug() << "comparing" << file << (file.lastModified() == (*wit).second)
            //          << QDateTime::fromTime_t(file.lastModified());
            if (!file.exists() || file.lastModified() != (*wit).second) {
                const quint32 fileId = Location::fileId(file);
                dirtyFiles.insert(fileId);
                mVisitedFiles.remove(fileId);
                pending.append(file);
                wit = it.value().erase(wit);
                wend = it.value().end(); // ### do we need to update 'end' here?

                DependencyHash::const_iterator dit = mDependencies.find(fileId);
                if (dit == mDependencies.end()) {
                    error() << "file modified but not in dependency list" << file;
                    ++it;
                    continue;
                }
                Q_ASSERT(!dit.value().isEmpty());
                foreach (quint32 pathId, dit.value()) {
                    dirtyFiles.insert(pathId);
                    mVisitedFiles.remove(pathId);
                    const Path path = Location::path(pathId);
                    if (path.exists()) {
                        bool ok;
                        char buf[4];
                        memcpy(buf, &pathId, 4);
                        const FileInformation fi = db->value<FileInformation>(Slice(buf, 4), &ok);
                        if (ok) {
                            if (Rdm::isPch(fi.compileArgs)) {
                                toIndexPch[path] = fi.compileArgs;
                            } else {
                                toIndex[path] = fi.compileArgs;
                            }
                        }
                    }
                }
            } else {
                ++wit;
            }
        }

        foreach(const Path &path, pending) {
            it.value().insert(qMakePair<QByteArray, time_t>(path.fileName(), path.lastModified()));
        }
    }
    if (toIndex.isEmpty() && toIndexPch.isEmpty())
        return;

    if (dirtyFiles.size() > 1) {
        Server::instance()->threadPool()->start(new DirtyJob(this, dirtyFiles, toIndexPch, toIndex));
    } else {
        for (QHash<Path, QList<QByteArray> >::const_iterator it = toIndexPch.begin(); it != toIndexPch.end(); ++it) {
            index(it.key(), it.value(), IndexerJob::DirtyPch|IndexerJob::NeedsDirty);
        }

        for (QHash<Path, QList<QByteArray> >::const_iterator it = toIndex.begin(); it != toIndex.end(); ++it) {
            index(it.key(), it.value(), IndexerJob::Dirty|IndexerJob::NeedsDirty);
        }
    }
}

void Indexer::onJobComplete(int id, const Path &input, bool isPch, const QByteArray &msg)
{
    Q_UNUSED(input);

    MutexLocker locker(&mMutex);
    mJobs.remove(id);
    mIndexing.remove(input);
    if (isPch) {
        QHash<int, IndexerJob*>::iterator it = mWaitingForPCH.begin();
        while (it != mWaitingForPCH.end()) {
            IndexerJob *job = it.value();
            if (!needsToWaitForPch(job)) {
                const int id = it.key();
                it = mWaitingForPCH.erase(it);
                startJob(id, job);
            } else {
                ++it;
            }
        }
    }
    const int idx = mJobCounter - (mIndexing.size() + mWaitingForPCH.size());
    error("[%3d%%] %d/%d %s. Pending jobs %d. %lld mb mem.",
          static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
          msg.constData(), mJobs.size() + mWaitingForPCH.size(),
          (MemoryMonitor::usage() / (1024 * 1024)));

    if (mJobs.isEmpty()) {
        Q_ASSERT(mTimerRunning);
        mTimerRunning = false;
        error() << "jobs took" << ((double)(mTimer.elapsed()) / 1000.0) << "secs"
                << "using" << (double(MemoryMonitor::usage()) / (1024.0 * 1024.0)) << "mb of memory";
        mJobCounter = 0;
        emit jobsComplete();
    }

    emit indexingDone(id);
    sender()->deleteLater();
}

void Indexer::setPchDependencies(const Path &pchHeader, const QSet<quint32> &deps)
{
    QWriteLocker lock(&mPchDependenciesLock);
    if (deps.isEmpty()) {
        mPchDependencies.remove(pchHeader);
    } else {
        mPchDependencies[pchHeader] = deps;
    }
    Rdm::writePchDepencies(mPchDependencies);
}

QSet<quint32> Indexer::pchDependencies(const Path &pchHeader) const
{
    QReadLocker lock(&mPchDependenciesLock);
    return mPchDependencies.value(pchHeader);
}

void Indexer::addDependencies(const DependencyHash &deps)
{
    MutexLocker lock(&mMutex);
    commitDependencies(deps, true);
}

PchUSRHash Indexer::pchUSRHash(const QList<Path> &pchFiles) const
{
    QReadLocker lock(&mPchUSRHashLock);
    const int count = pchFiles.size();
    switch (pchFiles.size()) {
    case 0: return PchUSRHash();
    case 1: return mPchUSRHashes.value(pchFiles.first());
    default:
        break;
    }
    PchUSRHash ret = mPchUSRHashes.value(pchFiles.first());
    for (int i=1; i<count; ++i) {
        const PchUSRHash h = mPchUSRHashes.value(pchFiles.at(i));
        for (PchUSRHash::const_iterator it = h.begin(); it != h.end(); ++it) {
            ret[it.key()] = it.value();
        }
    }
    return ret;
}

void Indexer::setPchUSRHash(const Path &pch, const PchUSRHash &astHash)
{
    QWriteLocker lock(&mPchUSRHashLock);
    mPchUSRHashes[pch] = astHash;
    Rdm::writePchUSRHashes(mPchUSRHashes);
}
bool Indexer::needsToWaitForPch(IndexerJob *job) const
{
    foreach(const Path &pchHeader, job->mPchHeaders) {
        if (mIndexing.contains(pchHeader))
            return true;
    }
    return false;
}

void Indexer::abort()
{
    MutexLocker lock(&mMutex);
    qDeleteAll(mWaitingForPCH);
    mWaitingForPCH.clear();
    foreach(IndexerJob *job, mJobs) {
        job->abort();
    }
}

QByteArray Indexer::fixIts(const Path &path) const
{
    quint32 fileId = Location::fileId(path);
    if (!fileId)
        return QByteArray();
    QReadLocker lock(&mFixItsAndErrorsLock);
    QMap<Location, QPair<int, QByteArray> >::const_iterator it = mFixIts.lowerBound(Location(fileId, 0));
    QByteArray ret;
    char buf[1024];
    while (it != mFixIts.end() && it.key().fileId() == fileId) {
        int w;
        if (it.value().first) {
            w = snprintf(buf, sizeof(buf), "%d-%d %s%s", it.key().offset(), it.value().first,
                         it.value().second.constData(), ret.isEmpty() ? "" : "\n");
        } else {
            w = snprintf(buf, sizeof(buf), "%d %s%s", it.key().offset(),
                         it.value().second.constData(), ret.isEmpty() ? "" : "\n");
        }
        ret.prepend(QByteArray(buf, w)); // we want the last ones first
        ++it;
    }
    return ret;
}

QByteArray Indexer::errors(const Path &path) const
{
    quint32 fileId = Location::fileId(path);
    if (!fileId)
        return QByteArray();
    QReadLocker lock(&mFixItsAndErrorsLock);
    return mErrors.value(fileId);
}


void Indexer::setDiagnostics(const QHash<quint32, QList<QByteArray> > &diagnostics,
                             const QMap<Location, QPair<int, QByteArray> > &fixIts)
{
    QWriteLocker lock(&mFixItsAndErrorsLock);

    for (QHash<quint32, QList<QByteArray> >::const_iterator it = diagnostics.begin(); it != diagnostics.end(); ++it) {
        const quint32 fileId = it.key();
        QMap<Location, QPair<int, QByteArray> >::iterator i = mFixIts.lowerBound(Location(fileId, 0));
        while (i != mFixIts.end() && i.key().fileId() == fileId) {
            i = mFixIts.erase(i);
        }
        if (it.value().isEmpty()) {
            mErrors.remove(it.key());
        } else {
            mErrors[it.key()] = RTags::join(it.value(), "\n");
        }
    }
    for (QMap<Location, QPair<int, QByteArray> >::const_iterator it = fixIts.begin(); it != fixIts.end(); ++it) {
        mFixIts[it.key()] = it.value();
    }
}

void Indexer::reindex(const QByteArray &pattern)
{
    initDB(ForceDirty, pattern);
}
