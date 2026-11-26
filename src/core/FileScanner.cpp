#include "core/FileScanner.h"

#include <QDir>
#include <QFileInfo>

namespace bv {

// ---------- NodeStack ----------

NodeStack::~NodeStack()
{
    while (!isEmpty())
        pop();
}

void NodeStack::push(const FileNode *node)
{
    // The new item points to the old top, then becomes the top.
    m_top = new Item{node, m_top};
}

const FileNode *NodeStack::pop()
{
    Item *old = m_top;
    const FileNode *node = old->node;
    m_top = old->next;
    delete old;
    return node;
}

bool NodeStack::isEmpty() const
{
    return m_top == nullptr;
}

// ---------- NodeQueue ----------

NodeQueue::~NodeQueue()
{
    while (!isEmpty())
        dequeue();
}

void NodeQueue::enqueue(const FileNode *node)
{
    Item *item = new Item{node, nullptr};
    if (m_back == nullptr) {
        m_front = item;  // queue was empty
        m_back = item;
    } else {
        m_back->next = item;
        m_back = item;
    }
}

const FileNode *NodeQueue::dequeue()
{
    Item *old = m_front;
    const FileNode *node = old->node;
    m_front = old->next;
    if (m_front == nullptr)
        m_back = nullptr;  // queue is now empty
    delete old;
    return node;
}

bool NodeQueue::isEmpty() const
{
    return m_front == nullptr;
}

// ---------- FileScanner ----------

Result<FileNode> FileScanner::scan(const QString &rootPath)
{
    m_warnings.clear();

    QFileInfo rootInfo(rootPath);
    if (!rootInfo.exists())
        return Result<FileNode>::failure(QString("Folder does not exist: ") + rootPath);
    if (!rootInfo.isDir())
        return Result<FileNode>::failure(QString("Not a folder: ") + rootPath);
    if (!rootInfo.isReadable())
        return Result<FileNode>::failure(QString("Cannot read folder: ") + rootPath);

    return Result<FileNode>::success(buildDirectoryNode(rootInfo));
}

// Recursive step: build one folder node, then build a child node for each
// entry inside it (sub-folders call this function again).
FileNode FileScanner::buildDirectoryNode(const QFileInfo &dirInfo)
{
    const QString path = dirInfo.absoluteFilePath();
    QString name = dirInfo.fileName();
    if (name.isEmpty())
        name = path;  // a drive root such as "E:/" has no file name

    FileNode node(name, path, true);

    if (!dirInfo.isReadable()) {
        m_warnings.push_back(QString("Cannot read folder, left empty: ") + path);
        return node;
    }

    // Name order, folders first, so results are the same on every run.
    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name | QDir::DirsFirst);

    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink()) {
            m_warnings.push_back(QString("Skipped symbolic link: ") + entry.absoluteFilePath());
            continue;
        }
        if (entry.isDir())
            node.addChild(buildDirectoryNode(entry));
        else
            node.addChild(FileNode(entry.fileName(), entry.absoluteFilePath(), false));
    }
    return node;
}

// DFS: pop a node, record it, push its children. Children are pushed in
// reverse so the first child is on top and is visited first.
std::vector<QString> FileScanner::traverseDFS(const FileNode &root) const
{
    std::vector<QString> order;
    NodeStack stack;
    stack.push(&root);

    while (!stack.isEmpty()) {
        const FileNode *current = stack.pop();
        order.push_back(current->path());

        const std::vector<FileNode> &kids = current->children();
        for (std::size_t i = kids.size(); i > 0; --i)
            stack.push(&kids[i - 1]);
    }
    return order;
}

// BFS: dequeue a node, record it, enqueue its children in order.
std::vector<QString> FileScanner::traverseBFS(const FileNode &root) const
{
    std::vector<QString> order;
    NodeQueue queue;
    queue.enqueue(&root);

    while (!queue.isEmpty()) {
        const FileNode *current = queue.dequeue();
        order.push_back(current->path());

        for (const FileNode &child : current->children())
            queue.enqueue(&child);
    }
    return order;
}

const std::vector<QString> &FileScanner::warnings() const
{
    return m_warnings;
}

}  // namespace bv
