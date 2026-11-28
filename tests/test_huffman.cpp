#include <iostream>
#include <string>

#include "compression/Huffman.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << std::endl;
        ++failures;
    }
}

static Bytes bytesOf(const std::string &text)
{
    return Bytes(text.begin(), text.end());
}

// Simple repeatable pseudo-random bytes (same on every platform).
static Bytes pseudoRandomBytes(std::size_t size, std::uint32_t seed)
{
    Bytes data(size);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

// Encodes and decodes `input` and checks that the same bytes come back.
static void checkRoundTrip(const Bytes &input, const char *what)
{
    const auto encoded = Huffman::encode(input);
    check(encoded.isOk(), what);
    if (!encoded.isOk())
        return;
    const auto decoded = Huffman::decode(encoded.value(), input.size());
    check(decoded.isOk(), what);
    check(decoded.isOk() && decoded.value() == input, what);
}

int main()
{
    // ---- Round trips -------------------------------------------------
    checkRoundTrip(Bytes(), "round trip: empty input");
    checkRoundTrip(Bytes{42}, "round trip: one byte");
    checkRoundTrip(Bytes(1000, 0x00), "round trip: one value repeated (zeros)");
    checkRoundTrip(Bytes(65536, 0xFF), "round trip: full chunk of one value");
    checkRoundTrip(bytesOf("ab"), "round trip: two different bytes");
    checkRoundTrip(bytesOf("abracadabra abracadabra"), "round trip: short text");
    checkRoundTrip(pseudoRandomBytes(65536, 1), "round trip: random full chunk");
    checkRoundTrip(pseudoRandomBytes(777, 2), "round trip: random odd size");

    Bytes allValues;
    for (int round = 0; round < 3; ++round) {
        for (int value = 0; value < 256; ++value)
            allValues.push_back(static_cast<std::uint8_t>(value));
    }
    checkRoundTrip(allValues, "round trip: every byte value present");

    // A deep tree: value i appears about 2^i times (kept small so the test
    // is fast), which forces long codes for the rarest values.
    Bytes skewed;
    for (int value = 0; value < 14; ++value) {
        const std::size_t count = std::size_t(1) << value;
        skewed.insert(skewed.end(), count, static_cast<std::uint8_t>(value));
    }
    checkRoundTrip(skewed, "round trip: skewed frequencies (deep tree)");

    // ---- Known answer -------------------------------------------------
    // "aab": 'a' (97) and 'b' (98) both get 1-bit codes. Canonical codes:
    // a = 0, b = 1. Bits 0 0 1 + five zero padding bits = 0x20.
    {
        const auto encoded = Huffman::encode(bytesOf("aab"));
        check(encoded.isOk() && encoded.value().size() == 257,
              "known answer: size is table + 1 byte");
        if (encoded.isOk() && encoded.value().size() == 257) {
            const Bytes &data = encoded.value();
            bool tableOk = true;
            for (int i = 0; i < 256; ++i) {
                const std::uint8_t expected = (i == 97 || i == 98) ? 1 : 0;
                if (data[i] != expected)
                    tableOk = false;
            }
            check(tableOk, "known answer: code length table");
            check(data[256] == 0x20, "known answer: bit stream");
        }
    }

    // ---- Shape of the output -----------------------------------------
    {
        const auto empty = Huffman::encode(Bytes());
        check(empty.isOk() && empty.value() == Bytes(256, 0),
              "empty input encodes to an all-zero table");

        // One repeated value: 1 bit per byte, so 8 bytes -> 1 data byte.
        const auto single = Huffman::encode(Bytes(8, 7));
        check(single.isOk() && single.value().size() == 257
                  && single.value()[7] == 1 && single.value()[256] == 0x00,
              "single value gets a 1-bit code");

        // Encoding is deterministic.
        const Bytes data = pseudoRandomBytes(5000, 3);
        const auto first = Huffman::encode(data);
        const auto second = Huffman::encode(data);
        check(first.isOk() && second.isOk() && first.value() == second.value(),
              "encode is deterministic");
    }

    // ---- Compression actually happens --------------------------------
    {
        Bytes text;
        const std::string sentence = "the quick brown fox jumps over the lazy dog. ";
        while (text.size() < 20000)
            text.insert(text.end(), sentence.begin(), sentence.end());
        const auto encoded = Huffman::encode(text);
        check(encoded.isOk() && encoded.value().size() < text.size(),
              "repetitive text gets smaller");

        const auto zeros = Huffman::encode(Bytes(65536, 0));
        check(zeros.isOk() && zeros.value().size() == 256 + 8192,
              "65536 equal bytes -> table + 8192 bytes");

        // Random data cannot be compressed; the caller must be able to see
        // that and fall back to raw storage.
        const Bytes random = pseudoRandomBytes(65536, 4);
        const auto randomEncoded = Huffman::encode(random);
        check(randomEncoded.isOk() && randomEncoded.value().size() >= random.size(),
              "random data does not shrink");
    }

    // ---- Corrupt or invalid input is rejected ------------------------
    const Bytes original = bytesOf("hello huffman hello huffman hello world");
    const auto good = Huffman::encode(original);
    check(good.isOk(), "encode fixture");
    if (!good.isOk())
        return 1;
    const Bytes &goodData = good.value();

    check(Huffman::decode(goodData, original.size()).isOk(),
          "fixture decodes");

    {
        Bytes shortTable(goodData.begin(), goodData.begin() + 100);
        check(!Huffman::decode(shortTable, original.size()).isOk(),
              "truncated table rejected");
        check(!Huffman::decode(Bytes(), 0).isOk(),
              "completely empty input rejected");
    }
    {
        Bytes truncated(goodData.begin(), goodData.end() - 1);
        check(!Huffman::decode(truncated, original.size()).isOk(),
              "truncated bit stream rejected");
    }
    {
        Bytes extra = goodData;
        extra.push_back(0x00);
        check(!Huffman::decode(extra, original.size()).isOk(),
              "extra byte after the data rejected");
    }
    check(!Huffman::decode(goodData, original.size() + 100).isOk(),
          "original size too large rejected");
    check(!Huffman::decode(goodData, original.size() - 10).isOk(),
          "original size too small rejected");
    check(!Huffman::decode(goodData, 0).isOk(),
          "size 0 with non-empty table rejected");
    {
        // Padding bits must be zero. "aab" uses 3 bits of its last byte.
        Bytes data = Huffman::encode(bytesOf("aab")).value();
        data[256] = 0x21;
        check(!Huffman::decode(data, 3).isOk(), "non-zero padding rejected");
    }
    {
        // Table that is not a valid prefix code.
        Bytes oversubscribed(256, 0);
        oversubscribed[0] = oversubscribed[1] = oversubscribed[2] = 1;
        oversubscribed.push_back(0x00);
        check(!Huffman::decode(oversubscribed, 1).isOk(),
              "oversubscribed table rejected");

        Bytes incomplete(256, 0);
        incomplete[0] = 1;
        incomplete[1] = 2;  // codes 0 and 10 only; 11 is unused
        incomplete.push_back(0x00);
        check(!Huffman::decode(incomplete, 1).isOk(),
              "incomplete table rejected");

        Bytes tooLong(256, 0);
        tooLong[0] = 33;
        tooLong.push_back(0x00);
        check(!Huffman::decode(tooLong, 1).isOk(),
              "code length above 32 rejected");

        Bytes singleWrongLength(256, 0);
        singleWrongLength[5] = 3;
        singleWrongLength.push_back(0x00);
        check(!Huffman::decode(singleWrongLength, 1).isOk(),
              "single symbol with a 3-bit code rejected");

        // Empty table but the caller expects bytes.
        check(!Huffman::decode(Bytes(256, 0), 5).isOk(),
              "empty table with non-zero size rejected");
        check(Huffman::decode(Bytes(256, 0), 0).isOk(),
              "empty table with size 0 accepted");
        check(!Huffman::decode(Bytes(257, 0), 0).isOk(),
              "empty table followed by data rejected");
    }
    {
        // Single-symbol data: a '1' bit is not a valid code.
        Bytes data(256, 0);
        data[9] = 1;
        data.push_back(0x80);
        check(!Huffman::decode(data, 1).isOk(),
              "invalid bit in single-symbol stream rejected");
    }
    {
        // Huge claimed size must fail fast, not allocate.
        check(!Huffman::decode(goodData, std::size_t(1) << 40).isOk(),
              "absurd original size rejected");
    }
    {
        // Flipping bits in the bit stream must never crash. It may decode
        // to different bytes (the chunk hash check catches that), or fail.
        for (std::size_t i = 256; i < goodData.size(); ++i) {
            Bytes damaged = goodData;
            damaged[i] ^= 0xFF;
            const auto decoded = Huffman::decode(damaged, original.size());
            check(!decoded.isOk() || decoded.value() != original,
                  "damaged byte does not decode to the original");
        }
    }

    if (failures == 0)
        std::cout << "All Huffman checks passed." << std::endl;

    return failures == 0 ? 0 : 1;
}
