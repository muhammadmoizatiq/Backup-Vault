#pragma once

#include <QString>
#include <functional>
#include <vector>

#include "common/Types.h"
#include "common/Result.h"

namespace bv {

class Chunker {
public:
    using ChunkCallback = std::function<Status(const Bytes &)>;

    // Reads one chunk at a time. Stops immediately if the callback fails.
    static Status streamFile(
        const QString &filePath,
        const ChunkCallback &callback
    );

    // Kept for compatibility with existing code and tests.
    static Result<std::vector<Bytes>> splitFile(
        const QString &filePath
    );
};

} // namespace bv