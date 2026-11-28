#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "compression/MinHeap.h"

namespace bv {

constexpr int kHuffmanSymbolCount = 256;  // one symbol per byte value
constexpr int kHuffmanMaxCodeLength = 32;

// Code length in bits for every byte value (0 = value not used).
using HuffmanLengths = std::array<std::uint8_t, kHuffmanSymbolCount>;

// A binary Huffman tree (BV-32). Bit 0 goes left, bit 1 goes right.
// It is used in two ways:
//   Encoder: buildFromFrequencies() + codeLengths().
//   Decoder: addCode() once per symbol, then walk with child()/isLeaf().
// Do not mix the two on one object; call clear() in between.
class HuffmanTree {
public:
    void clear();

    // ---- Encoder side ----------------------------------------------
    // Builds the tree with a Min Heap: repeatedly join the two least
    // frequent nodes. Symbols with frequency 0 are left out.
    void buildFromFrequencies(const std::array<std::uint64_t, kHuffmanSymbolCount> &frequency);

    // Writes each symbol's depth (= code length) into `lengths`.
    // A tree with a single symbol gives that symbol length 1.
    // Returns false if some code is longer than `maxLength`.
    bool codeLengths(HuffmanLengths &lengths, int maxLength) const;

    // ---- Decoder side ----------------------------------------------
    // Inserts a code (the lowest `length` bits of `code`, most significant
    // first). Returns false if it collides with a code already added:
    // the same code, or one code being the start of another.
    bool addCode(int symbol, std::uint32_t code, int length);

    // ---- Walking the tree ------------------------------------------
    // Index of the root (valid only if the tree is not empty).
    int root() const { return m_root; }
    // Child of `node` for this bit, or kNoNode if there is none.
    int child(int node, bool bit) const;
    bool isLeaf(int node) const;
    int symbolAt(int node) const;

private:
    std::vector<HuffmanNode> m_nodes;
    int m_root = kNoNode;
};

}  // namespace bv
