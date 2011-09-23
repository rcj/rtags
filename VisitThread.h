#ifndef VisitThread_h
#define VisitThread_h

#include <QtCore>
#include "Path.h"
#include "Location.h"

struct Node;
struct CursorNode;
class VisitThread : public QThread
{
    Q_OBJECT
public:
    VisitThread();
    ~VisitThread();
    void printTree();
    QSet<Path> files() const;
    void abort();
    void lockMutex() { mMutex.lock(); }
    void unlockMutex() { mMutex.unlock(); }
    Node *nodeForLocation(const Location &loc) const;
    bool save(const QByteArray &file);
signals:
    void done();
public slots:
    void invalidate(const QSet<Path> &paths);
    void onFileParsed(const Path &path, void *unit);
    void onParseError(const Path &path);
private:
    struct PendingReference {
        CursorNode *node;
        Location location;
    };


    void buildTree(Node *node, CursorNode *c, QHash<QByteArray, PendingReference> &references);
    void addReference(CursorNode *c, const QByteArray &id, const Location &location);

    Node *mRoot;
    mutable QMutex mMutex;
    QMap<QByteArray, Node*> mNodes;
    bool mQuitting;
    int32_t mLongestId;
};

#endif
