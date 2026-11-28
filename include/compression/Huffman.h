#pragma once

#include <cstddef>

#include "common/Result.h"
#include "common/Types.h"

namespace bv {

// Canonical Huffman coding for arbitrary binary data (BV-09).
//
// Encoded layout (this is the "payload" of a Huffman chunk, see
// docs/ChunkFormat.md):
//   bytes 0..255 : code length in bits for each byte value 0..255
//                  (0 = that value does not occur in the data)
//   bytes 256..  : the codes of all input bytes, packed most significant
//                  bit first; unused bits in the last byte are 0
//
// Codes are rebuilt from the lengths alone ("canonical" codes), so no tree
// has to be stored. The original size is NOT stored here: the caller keeps
// it (the chunk header) and passes it to decode().
class Huffman {
public:
    // Compresses `input`. Empty and single-value input are supported.
    // Fails only if a code would be longer than 32 bits (not possible for
    // chunk-sized input). Note: the result can be larger than the input;
    // the caller decides whether compression is worth using.
    static Result<Bytes> encode(const Bytes &input);

    // Decompresses `encoded` into exactly `originalSize` bytes.
    // Fails if the table is invalid, the data is truncated, there is extra
    // data after the last code, or the padding bits are not zero.
    static Result<Bytes> decode(const Bytes &encoded, std::size_t originalSize);
};

}  // namespace bv
