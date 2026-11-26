#pragma once

#include <QString>
#include <vector>

#include "common/Result.h"
#include "core/FileNode.h"

class QFileInfo;

namespace bv {

// Stack (last in, first out), built by hand as a linked list.
// Used by FileScanner::traverseDFS. The stack does not own the nodes it holds.
class NodeStack {
public:
    NodeStack() = default;
    ~NodeStack();
    NodeStack(const NodeStack &) = delete;
    NodeStack &operator=(const NodeStack &) = delete;

    void push(const FileNode *node);
    const FileNode *pop();  // call only when isEmpty() is false
    bool isEmpty() const;

private:
    struct Item {
        const FileNode *node;
        Item *next;
    };
    Item *m_top = nullptr;
};

// Queue (first in, first out), built by hand as a linked list.
// Used by FileScanner::traverseBFS. The queue does not own the nodes it holds.
class NodeQueue {
public:
    NodeQueue() = default;
    ~NodeQueue();
    NodeQueue(const NodeQueue &) = delete;
    NodeQueue &operator=(const NodeQueue &) = delete;

    void enqueue(const FileNode *node);
    const FileNode *dequeue();  // call only when isEmpty() is false
    bool isEmpty() const;

private:
    struct Item {
        const FileNode *node;
        Item *next;
    };
    Item *m_front = nullptr;
    Item *m_back = nullptr;
};

class FileScanner {
public:
    // Builds the N-ary Tree for a folder. Fails if the folder is missing,
    // is not a folder, or cannot be read. Empty folders are kept as nodes.
    // Symbolic links and unreadable sub-folders are skipped/left empty and
    // reported through warnings().
    Result<FileNode> scan(const QString &rootPath);

    // Paths in depth-first order (Stack). A folder comes before its contents.
    std::vector<QString> traverseDFS(const FileNode &root) const;

    // Paths in breadth-first order (Queue). Level by level.
    std::vector<QString> traverseBFS(const FileNode &root) const;

    // Warnings from the most recent scan().
    const std::vector<QString> &warnings() const;

private:
    FileNode buildDirectoryNode(const QFileInfo &dirInfo);

    std::vector<QString> m_warnings;
};

}  // namespace bv
