
#pragma once

#include <QString>
#include <vector>

namespace bv {

class FileNode {
public:
    // Default constructor
    FileNode() = default;

    // Initialize a file or directory node
    FileNode(const QString &name, const QString &path, bool isDirectory);

    // Add a child node to this node
    void addChild(FileNode child);

    // Get node information
    const QString &name() const;
    const QString &path() const;
    bool isDirectory() const;

    // Get all child nodes
    const std::vector<FileNode> &children() const;

private:
    QString m_name;
    QString m_path;
    bool m_isDirectory = false;

    // N-ary Tree: each node can have multiple children
    std::vector<FileNode> m_children;
};

} // namespace bv
