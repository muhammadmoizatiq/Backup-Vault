#include "compression/HuffmanTree.h"

#include <utility>

namespace bv {

void HuffmanTree::clear()
{
    m_nodes.clear();
    m_root = kNoNode;
}

void HuffmanTree::buildFromFrequencies(
    const std::array<std::uint64_t, kHuffmanSymbolCount> &frequency)
{
    clear();
    m_nodes.reserve(2 * kHuffmanSymbolCount);  // keeps references stable
    MinHeap heap(m_nodes);

    for (int symbol = 0; symbol < kHuffmanSymbolCount; ++symbol) {
        if (frequency[symbol] == 0)
            continue;
        m_nodes.push_back({frequency[symbol], kNoNode, kNoNode, symbol});
        heap.push(static_cast<int>(m_nodes.size()) - 1);
    }
    if (heap.isEmpty())
        return;

    while (heap.size() > 1) {
        const int first = heap.pop();
        const int second = heap.pop();
        m_nodes.push_back({m_nodes[first].frequency + m_nodes[second].frequency,
                           first, second, kNotASymbol});
        heap.push(static_cast<int>(m_nodes.size()) - 1);
    }
    m_root = heap.pop();
}

bool HuffmanTree::codeLengths(HuffmanLengths &lengths, int maxLength) const
{
    lengths.fill(0);
    if (m_root == kNoNode)
        return true;

    // One distinct symbol: the tree is a single leaf. Give it a 1-bit code
    // so that every input byte still produces at least one bit.
    if (isLeaf(m_root)) {
        lengths[m_nodes[m_root].symbol] = 1;
        return maxLength >= 1;
    }

    std::vector<std::pair<int, int>> pending;  // (node index, depth)
    pending.push_back({m_root, 0});
    while (!pending.empty()) {
        const std::pair<int, int> current = pending.back();
        pending.pop_back();
        const HuffmanNode &node = m_nodes[current.first];
        if (node.symbol != kNotASymbol) {
            if (current.second > maxLength)
                return false;
            lengths[node.symbol] = static_cast<std::uint8_t>(current.second);
        } else {
            pending.push_back({node.left, current.second + 1});
            pending.push_back({node.right, current.second + 1});
        }
    }
    return true;
}

bool HuffmanTree::addCode(int symbol, std::uint32_t code, int length)
{
    if (length < 1 || length > kHuffmanMaxCodeLength)
        return false;

    if (m_nodes.empty()) {
        m_nodes.push_back({0, kNoNode, kNoNode, kNotASymbol});
        m_root = 0;
    }

    int current = m_root;
    for (int bitIndex = length - 1; bitIndex >= 0; --bitIndex) {
        if (m_nodes[current].symbol != kNotASymbol)
            return false;  // an earlier code is a prefix of this one
        const bool goRight = ((code >> bitIndex) & 1) != 0;
        int next = goRight ? m_nodes[current].right : m_nodes[current].left;
        if (next == kNoNode) {
            m_nodes.push_back({0, kNoNode, kNoNode, kNotASymbol});
            next = static_cast<int>(m_nodes.size()) - 1;
            if (goRight)
                m_nodes[current].right = next;
            else
                m_nodes[current].left = next;
        }
        current = next;
    }

    // The final node must be new: not a leaf and without children
    // (children would mean this code is the start of an earlier one).
    if (m_nodes[current].symbol != kNotASymbol || m_nodes[current].left != kNoNode
        || m_nodes[current].right != kNoNode) {
        return false;
    }
    m_nodes[current].symbol = symbol;
    return true;
}

int HuffmanTree::child(int node, bool bit) const
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size()))
        return kNoNode;
    return bit ? m_nodes[node].right : m_nodes[node].left;
}

bool HuffmanTree::isLeaf(int node) const
{
    return node >= 0 && node < static_cast<int>(m_nodes.size())
           && m_nodes[node].symbol != kNotASymbol;
}

int HuffmanTree::symbolAt(int node) const
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size()))
        return kNotASymbol;
    return m_nodes[node].symbol;
}

}  // namespace bv
