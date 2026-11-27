
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/Result.h"
#include "database/DatabaseManager.h"

namespace bv {

// SHA-256 digest represented as 32 raw bytes in memory.
using ChunkHash = std::array<std::uint8_t, 32>;

// In-memory cache of database chunk records.
// SQLite remains the authoritative source of persisted metadata.
class ChunkIndex {
public:
    explicit ChunkIndex(DatabaseManager &database,
                        std::size_t bucketCount = 101);

    ~ChunkIndex();

    ChunkIndex(const ChunkIndex &) = delete;
    ChunkIndex &operator=(const ChunkIndex &) = delete;

    // Calculate SHA-256 for arbitrary binary chunk data.
    static ChunkHash hashBytes(const Bytes &raw);

    // Convert a 32-byte hash to 64 lowercase hexadecimal characters.
    static QString toHex(const ChunkHash &hash);

    // Find a chunk in memory, or load it from SQLite on a cache miss.
    Result<ChunkRecord> find(const ChunkHash &hash);

    // Check whether a chunk exists in memory or SQLite.
    Result<bool> exists(const ChunkHash &hash);

    // Insert or update a record in the in-memory cache.
    // Does not write to SQLite or change reference counts.
    Status insert(const ChunkRecord &record);

    // Remove an entry from the in-memory cache only.
    // The SQLite record and physical chunk file are not deleted.
    bool remove(const ChunkHash &hash);

    // Remove every cached entry.
    void clear();

    // Number of records currently cached in memory.
    std::size_t size() const;

private:
    struct Node {
        ChunkHash hash;
        ChunkRecord record;
        Node *next;

        Node(const ChunkHash &key,
             const ChunkRecord &value,
             Node *nextNode = nullptr)
            : hash(key), record(value), next(nextNode) {}
    };

    std::size_t bucketFor(const ChunkHash &hash) const;

    Node *findNode(const ChunkHash &hash) const;

    static bool fromHex(const QString &hex, ChunkHash &hash);

    DatabaseManager &m_database;
    std::vector<Node *> m_buckets;
    std::size_t m_size;
};

} // namespace bv
