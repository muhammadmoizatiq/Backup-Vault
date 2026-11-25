#pragma once

#include <QString>

#include "common/Result.h"
#include "common/Types.h"

namespace bv {

// What the database needs to record after a chunk is stored.
struct StoredChunk {
    QString storagePath;       // relative to the vault root, e.g. "chunks/ab12..."
    std::uint64_t storedSize;  // bytes on disk, including the header
};

// Saves and loads chunk files under <vaultRoot>/chunks/.
// File name = 64-char lowercase hex SHA-256 of the raw chunk bytes.
// Format: see docs/ChunkFormat.md. Deleting chunks is NOT done here
// (BV-13 owns that, after the reference count reaches zero).
class ChunkStorage {
public:
    explicit ChunkStorage(const QString &vaultRoot);

    // Saves a chunk. If a valid file already exists, nothing is rewritten.
    // Fails if the hash is malformed or does not match the raw bytes.
    Result<StoredChunk> store(const QString &hashHex, const Bytes &raw) const;

    // Reads a chunk and returns the original raw bytes.
    // Fails if the file is missing, corrupt, truncated or has the wrong hash.
    Result<Bytes> load(const QString &hashHex) const;

    // True if a file for this hash exists (does not check that it is valid).
    bool exists(const QString &hashHex) const;

private:
    QString chunkFilePath(const QString &hashHex) const;
    QString chunksDir() const;

    QString m_vaultRoot;
};

}  // namespace bv
