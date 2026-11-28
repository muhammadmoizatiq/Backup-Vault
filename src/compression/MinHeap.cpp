#include "compression/MinHeap.h"

#include <utility>

namespace bv {

MinHeap::MinHeap(const std::vector<HuffmanNode> &nodes) : m_nodes(nodes) {}

bool MinHeap::isEmpty() const
{
    return m_items.empty();
}

std::size_t MinHeap::size() const
{
    return m_items.size();
}

void MinHeap::push(int nodeIndex)
{
    m_items.push_back(nodeIndex);
    moveUp(m_items.size() - 1);
}

int MinHeap::pop()
{
    if (m_items.empty())
        return kNoNode;

    const int smallest = m_items.front();
    m_items.front() = m_items.back();
    m_items.pop_back();
    if (!m_items.empty())
        moveDown(0);
    return smallest;
}

bool MinHeap::isSmaller(int a, int b) const
{
    if (m_nodes[a].frequency != m_nodes[b].frequency)
        return m_nodes[a].frequency < m_nodes[b].frequency;
    return a < b;
}

// Swap a new item upward while it is smaller than its parent.
void MinHeap::moveUp(std::size_t position)
{
    while (position > 0) {
        const std::size_t parent = (position - 1) / 2;
        if (!isSmaller(m_items[position], m_items[parent]))
            break;
        std::swap(m_items[position], m_items[parent]);
        position = parent;
    }
}

// Swap an item downward while a child is smaller than it.
void MinHeap::moveDown(std::size_t position)
{
    while (true) {
        const std::size_t left = 2 * position + 1;
        const std::size_t right = 2 * position + 2;
        std::size_t smallest = position;
        if (left < m_items.size() && isSmaller(m_items[left], m_items[smallest]))
            smallest = left;
        if (right < m_items.size() && isSmaller(m_items[right], m_items[smallest]))
            smallest = right;
        if (smallest == position)
            break;
        std::swap(m_items[position], m_items[smallest]);
        position = smallest;
    }
}

}  // namespace bv
