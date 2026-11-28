#include "compression/Huffman.h"

#include <array>
#include <utility>
#include <vector>

namespace bv {

namespace {

constexpr int kSymbolCount = 256;     // one symbol per byte value
constexpr int kTableSize = 256;       // one length byte per symbol
constexpr int kMaxCodeLength = 32;    // codes are kept in 32-bit integers
constexpr int kNoNode = -1;
constexpr int kNotASymbol = -1;

using LengthTable = std::array<std::uint8_t, kSymbolCount>;
using CodeTable = std::array<std::uint32_t, kSymbolCount>;

// One node of a Huffman tree. Nodes live in a std::vector and refer to
// each other by index, so no raw pointers or manual delete are needed.
// A leaf has a symbol (0..255); an inner node has symbol == kNotASymbol.
struct TreeNode {
    std::uint64_t frequency;
    int left;
    int right;
    int symbol;
};

// Min Heap of node indices, written by hand (std::priority_queue is not
// allowed here). The node with the smallest frequency is at the top.
// Equal frequencies are ordered by node index so the tree, and therefore
// the encoded output, is the same on every run and platform.
class MinHeap {
public:
    explicit MinHeap(const std::vector<TreeNode> &nodes) : m_nodes(nodes) {}

    bool isEmpty() const { return m_items.empty(); }
    std::size_t size() const { return m_items.size(); }

    void push(int nodeIndex)
    {
        m_items.push_back(nodeIndex);
        moveUp(m_items.size() - 1);
    }

    // Removes and returns the smallest node index. Heap must not be empty.
    int pop()
    {
        const int smallest = m_items.front();
        m_items.front() = m_items.back();
        m_items.pop_back();
        if (!m_items.empty())
            moveDown(0);
        return smallest;
    }

private:
    bool isSmaller(int a, int b) const
    {
        if (m_nodes[a].frequency != m_nodes[b].frequency)
            return m_nodes[a].frequency < m_nodes[b].frequency;
        return a < b;
    }

    void moveUp(std::size_t i)
    {
        while (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (!isSmaller(m_items[i], m_items[parent]))
                break;
            std::swap(m_items[i], m_items[parent]);
            i = parent;
        }
    }

    void moveDown(std::size_t i)
    {
        while (true) {
            const std::size_t left = 2 * i + 1;
            const std::size_t right = 2 * i + 2;
            std::size_t smallest = i;
            if (left < m_items.size() && isSmaller(m_items[left], m_items[smallest]))
                smallest = left;
            if (right < m_items.size() && isSmaller(m_items[right], m_items[smallest]))
                smallest = right;
            if (smallest == i)
                break;
            std::swap(m_items[i], m_items[smallest]);
            i = smallest;
        }
    }

    const std::vector<TreeNode> &m_nodes;
    std::vector<int> m_items;
};

// Builds the Huffman tree for the given frequencies and writes each used
// symbol's depth (its code length) into `lengths`.
// Returns false if some code would be longer than kMaxCodeLength.
bool computeCodeLengths(const std::array<std::uint64_t, kSymbolCount> &frequency,
                        LengthTable &lengths)
{
    lengths.fill(0);

    std::vector<TreeNode> nodes;
    nodes.reserve(2 * kSymbolCount);
    MinHeap heap(nodes);

    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (frequency[symbol] == 0)
            continue;
        nodes.push_back({frequency[symbol], kNoNode, kNoNode, symbol});
        heap.push(static_cast<int>(nodes.size()) - 1);
    }

    // No symbols at all (empty input): there is nothing to build.
    if (heap.isEmpty())
        return true;

    // One distinct symbol: the tree is a single leaf. Give it a 1-bit code
    // so that every input byte still produces at least one bit.
    if (heap.size() == 1) {
        lengths[nodes[0].symbol] = 1;
        return true;
    }

    // Repeatedly join the two least frequent nodes.
    while (heap.size() > 1) {
        const int first = heap.pop();
        const int second = heap.pop();
        nodes.push_back({nodes[first].frequency + nodes[second].frequency,
                         first, second, kNotASymbol});
        heap.push(static_cast<int>(nodes.size()) - 1);
    }
    const int root = heap.pop();

    // Walk the tree; a leaf's depth is its code length.
    std::vector<std::pair<int, int>> pending;  // (node index, depth)
    pending.push_back({root, 0});
    while (!pending.empty()) {
        const std::pair<int, int> current = pending.back();
        pending.pop_back();
        const TreeNode &node = nodes[current.first];
        if (node.symbol != kNotASymbol) {
            if (current.second > kMaxCodeLength)
                return false;
            lengths[node.symbol] = static_cast<std::uint8_t>(current.second);
        } else {
            pending.push_back({node.left, current.second + 1});
            pending.push_back({node.right, current.second + 1});
        }
    }
    return true;
}

// Canonical code assignment. Shorter codes come first; codes of equal
// length are handed out in byte-value order. Encoder and decoder both use
// this, so the lengths alone are enough to rebuild every code.
// The lengths must already be checked (see tableIsValid).
void assignCanonicalCodes(const LengthTable &lengths, CodeTable &codes)
{
    std::array<std::uint64_t, kMaxCodeLength + 1> countPerLength{};
    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (lengths[symbol] > 0)
            ++countPerLength[lengths[symbol]];
    }

    std::array<std::uint64_t, kMaxCodeLength + 1> nextCode{};
    std::uint64_t code = 0;
    for (int length = 1; length <= kMaxCodeLength; ++length) {
        code = (code + countPerLength[length - 1]) << 1;
        nextCode[length] = code;
    }

    codes.fill(0);
    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (lengths[symbol] > 0)
            codes[symbol] = static_cast<std::uint32_t>(nextCode[lengths[symbol]]++);
    }
}

// Checks that a length table (read from untrusted data) describes a real
// prefix code that this module could have produced:
//  - no length above kMaxCodeLength;
//  - no symbols at all (empty data), or
//  - exactly one symbol with a 1-bit code, or
//  - two or more symbols whose codes fill the code space exactly
//    (Kraft sum = 1), i.e. a full binary tree.
Status tableIsValid(const LengthTable &lengths, int &usedSymbols)
{
    usedSymbols = 0;
    std::uint64_t kraftSum = 0;  // in units of 2^-kMaxCodeLength
    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        const int length = lengths[symbol];
        if (length == 0)
            continue;
        if (length > kMaxCodeLength)
            return Status::failure("Huffman table has a code longer than 32 bits.");
        ++usedSymbols;
        kraftSum += std::uint64_t(1) << (kMaxCodeLength - length);
    }

    const std::uint64_t fullSpace = std::uint64_t(1) << kMaxCodeLength;
    if (usedSymbols == 0)
        return Status::success();
    if (usedSymbols == 1) {
        if (kraftSum != fullSpace / 2)
            return Status::failure("Huffman table: a single symbol must have a 1-bit code.");
        return Status::success();
    }
    if (kraftSum != fullSpace)
        return Status::failure("Huffman table is not a valid prefix code.");
    return Status::success();
}

// Builds a decoding tree from canonical codes: bit 0 goes left, bit 1 goes
// right. Returns false if the codes do not form a proper prefix code.
bool buildDecodingTree(const LengthTable &lengths, const CodeTable &codes,
                       std::vector<TreeNode> &tree)
{
    tree.clear();
    tree.push_back({0, kNoNode, kNoNode, kNotASymbol});  // root

    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        const int length = lengths[symbol];
        if (length == 0)
            continue;

        int current = 0;
        for (int bitIndex = length - 1; bitIndex >= 0; --bitIndex) {
            if (tree[current].symbol != kNotASymbol)
                return false;  // an earlier code is a prefix of this one
            const bool goRight = ((codes[symbol] >> bitIndex) & 1) != 0;
            int child = goRight ? tree[current].right : tree[current].left;
            if (child == kNoNode) {
                tree.push_back({0, kNoNode, kNoNode, kNotASymbol});
                child = static_cast<int>(tree.size()) - 1;
                if (goRight)
                    tree[current].right = child;
                else
                    tree[current].left = child;
            }
            current = child;
        }
        // The final node must be new and must not have children.
        if (tree[current].symbol != kNotASymbol || tree[current].left != kNoNode
            || tree[current].right != kNoNode) {
            return false;
        }
        tree[current].symbol = symbol;
    }
    return true;
}

}  // namespace

Result<Bytes> Huffman::encode(const Bytes &input)
{
    std::array<std::uint64_t, kSymbolCount> frequency{};
    for (std::uint8_t value : input)
        ++frequency[value];

    LengthTable lengths;
    if (!computeCodeLengths(frequency, lengths))
        return Result<Bytes>::failure("Huffman code would be longer than 32 bits.");

    CodeTable codes;
    assignCanonicalCodes(lengths, codes);

    Bytes encoded(lengths.begin(), lengths.end());

    // Pack the codes most significant bit first.
    std::uint8_t currentByte = 0;
    int bitsInCurrentByte = 0;
    for (std::uint8_t value : input) {
        for (int bitIndex = lengths[value] - 1; bitIndex >= 0; --bitIndex) {
            currentByte = static_cast<std::uint8_t>(
                (currentByte << 1) | ((codes[value] >> bitIndex) & 1));
            if (++bitsInCurrentByte == 8) {
                encoded.push_back(currentByte);
                currentByte = 0;
                bitsInCurrentByte = 0;
            }
        }
    }
    if (bitsInCurrentByte > 0) {
        // Fill the rest of the last byte with zero bits.
        currentByte = static_cast<std::uint8_t>(currentByte << (8 - bitsInCurrentByte));
        encoded.push_back(currentByte);
    }
    return Result<Bytes>::success(encoded);
}

Result<Bytes> Huffman::decode(const Bytes &encoded, std::size_t originalSize)
{
    if (encoded.size() < static_cast<std::size_t>(kTableSize))
        return Result<Bytes>::failure("Huffman data is truncated (table incomplete).");

    LengthTable lengths;
    for (int symbol = 0; symbol < kSymbolCount; ++symbol)
        lengths[symbol] = encoded[symbol];

    int usedSymbols = 0;
    const Status tableStatus = tableIsValid(lengths, usedSymbols);
    if (!tableStatus.isOk())
        return Result<Bytes>::failure(tableStatus.message());

    const std::size_t dataBytes = encoded.size() - kTableSize;

    // Empty data: no symbols, no bits.
    if (usedSymbols == 0) {
        if (originalSize != 0 || dataBytes != 0)
            return Result<Bytes>::failure("Huffman data does not match the expected size.");
        return Result<Bytes>::success(Bytes());
    }
    if (originalSize == 0)
        return Result<Bytes>::failure("Huffman data does not match the expected size.");

    // Every symbol needs at least one bit. Checking this first also keeps
    // the output allocation below bounded by the size of the input.
    const std::uint64_t totalBits = static_cast<std::uint64_t>(dataBytes) * 8;
    if (originalSize > totalBits)
        return Result<Bytes>::failure("Huffman data is truncated (not enough bits).");

    CodeTable codes;
    assignCanonicalCodes(lengths, codes);

    std::vector<TreeNode> tree;
    if (!buildDecodingTree(lengths, codes, tree))
        return Result<Bytes>::failure("Huffman table is not a valid prefix code.");

    Bytes output;
    output.reserve(originalSize);
    std::uint64_t bitPosition = 0;
    int current = 0;  // start at the root
    while (output.size() < originalSize) {
        if (bitPosition >= totalBits)
            return Result<Bytes>::failure("Huffman data is truncated (ran out of bits).");

        const std::uint8_t byte = encoded[kTableSize + bitPosition / 8];
        const bool goRight = ((byte >> (7 - bitPosition % 8)) & 1) != 0;
        ++bitPosition;

        const int next = goRight ? tree[current].right : tree[current].left;
        if (next == kNoNode)
            return Result<Bytes>::failure("Huffman data contains an invalid code.");

        if (tree[next].symbol != kNotASymbol) {
            output.push_back(static_cast<std::uint8_t>(tree[next].symbol));
            current = 0;
        } else {
            current = next;
        }
    }

    // The data must end exactly where the last code ends.
    const std::uint64_t bytesUsed = (bitPosition + 7) / 8;
    if (bytesUsed != dataBytes)
        return Result<Bytes>::failure("Huffman data has extra bytes after the last code.");
    if (bitPosition % 8 != 0) {
        const std::uint8_t lastByte = encoded.back();
        const std::uint8_t paddingMask =
            static_cast<std::uint8_t>(0xFF >> (bitPosition % 8));
        if ((lastByte & paddingMask) != 0)
            return Result<Bytes>::failure("Huffman data has non-zero padding bits.");
    }

    return Result<Bytes>::success(output);
}

}  // namespace bv
