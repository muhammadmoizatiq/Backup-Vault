
#include "core/FileNode.h"

#include <utility>

namespace bv {

FileNode::FileNode(const QString &name,
                   const QString &path,
                   bool isDirectory)
    : m_name(name),
      m_path(path),
      m_isDirectory(isDirectory)
{
}

void FileNode::addChild(FileNode child)
{
    m_children.push_back(std::move(child));
}

const QString &FileNode::name() const
{
    return m_name;
}

const QString &FileNode::path() const
{
    return m_path;
}

bool FileNode::isDirectory() const
{
    return m_isDirectory;
}

const std::vector<FileNode> &FileNode::children() const
{
    return m_children;
}

} // namespace bv
