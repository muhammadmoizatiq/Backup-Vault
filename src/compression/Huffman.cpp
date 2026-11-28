#include "compression/Huffman.h"

#include <array>
#include <vector>

#include "compression/HuffmanTree.h"

namespace bv {

namespace {

constexpr int kSymbolCount = kHuffmanSymbolCount;
constexpr int kTableSize = 256;  // one length byte per symbol
constexpr int kMaxCodeLength = kHuffmanMaxCodeLength;

using LengthTable = HuffmanLengths;
using CodeTable = std::array<std::uint32_t, kSymbolCount>;

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

// Builds the decoding tree from canonical codes. Returns false if the
// codes do not form a proper prefix code.
bool buildDecodingTree(const LengthTable &lengths, const CodeTable &codes,
                       HuffmanTree &tree)
{
    tree.clear();
    for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (lengths[symbol] == 0)
            continue;
        if (!tree.addCode(symbol, codes[symbol], lengths[symbol]))
            return false;
    }
    return true;
}

}  // namespace

Result<Bytes> Huffman::encode(const Bytes &input)
{
    std::array<std::uint64_t, kSymbolCount> frequency{};
    for (std::uint8_t value : input)
        ++frequency[value];

    HuffmanTree tree;
    tree.buildFromFrequencies(frequency);

    LengthTable lengths;
    if (!tree.codeLengths(lengths, kMaxCodeLength))
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

    HuffmanTree tree;
    if (!buildDecodingTree(lengths, codes, tree))
        return Result<Bytes>::failure("Huffman table is not a valid prefix code.");

    Bytes output;
    output.reserve(originalSize);
    std::uint64_t bitPosition = 0;
    int current = tree.root();
    while (output.size() < originalSize) {
        if (bitPosition >= totalBits)
            return Result<Bytes>::failure("Huffman data is truncated (ran out of bits).");

        const std::uint8_t byte = encoded[kTableSize + bitPosition / 8];
        const bool goRight = ((byte >> (7 - bitPosition % 8)) & 1) != 0;
        ++bitPosition;

        const int next = tree.child(current, goRight);
        if (next == kNoNode)
            return Result<Bytes>::failure("Huffman data contains an invalid code.");

        if (tree.isLeaf(next)) {
            output.push_back(static_cast<std::uint8_t>(tree.symbolAt(next)));
            current = tree.root();
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
