#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bv {

// Arbitrary binary data (chunk contents are never treated as text).
using Bytes = std::vector<std::uint8_t>;

// Fixed chunk size used by the Chunker and checked by ChunkStorage.
constexpr std::size_t kChunkSize = 65536;

}  // namespace bv
