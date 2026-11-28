#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bv {

constexpr int kNoNode = -1;      // "no child" / "no node"
constexpr int kNotASymbol = -1;  // symbol value of an inner node

// One node of a Huffman tree. Nodes live in a std::vector and refer to
// each other by index, so no raw pointers or manual delete are needed.
// A leaf has a symbol (0..255); an inner node has symbol == kNotASymbol.
struct HuffmanNode {
    std::uint64_t frequency;
    int left;
    int right;
    int symbol;
};

// Min Heap of node indices (BV-32), written by hand: std::priority_queue
// is not allowed here. The node with the smallest frequency is on top.
// Nodes with equal frequency are ordered by index, so the Huffman tree
// (and the encoded output) is identical on every run and platform.
//
// The heap stores indices only. `nodes` must outlive the heap and every
// pushed index must be valid in it; nodes may be added to the vector later.
class MinHeap {
public:
    explicit MinHeap(const std::vector<HuffmanNode> &nodes);

    bool isEmpty() const;
    std::size_t size() const;

    void push(int nodeIndex);

    // Removes and returns the index of the smallest node,
    // or kNoNode if the heap is empty.
    int pop();

private:
    bool isSmaller(int a, int b) const;
    void moveUp(std::size_t position);
    void moveDown(std::size_t position);

    const std::vector<HuffmanNode> &m_nodes;
    std::vector<int> m_items;
};

}  // namespace bv
