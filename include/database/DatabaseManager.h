#pragma once

#include <QSqlDatabase>
#include <QString>

#include <vector>

#include "common/Result.h"
#include "common/Types.h"
#include "storage/ChunkStorage.h"

namespace bv {

// One row of the `chunks` table.
struct ChunkRecord {
    QString hash;                  // 64 lowercase hex characters
    std::uint64_t originalSize = 0;
    std::uint64_t storedSize = 0;  // bytes on disk, including the header
    QString storagePath;           // relative to the vault root
    std::int64_t refCount = 0;     // number of file_chunks rows using this chunk
    QString createdAt;             // UTC, ISO 8601
};

// Snapshot metadata read by the restore engine.
struct SnapshotRecord {
    std::int64_t id = 0;
    QString name;
    QString status;
};

// One file or directory stored in a snapshot.
struct FileEntryRecord {
    std::int64_t id = 0;
    std::int64_t snapshotId = 0;
    QString relativePath;
    QString entryType;
    std::uint64_t fileSize = 0;
    QString contentHash;
};

// Owns one SQLite connection (Qt SQL, QSQLITE) to <vaultRoot>/storage/backups/vault.db.
// Schema: docs/DatabaseSchema.md.
//
// Reference-count rule: every row in `file_chunks` counts as one reference.
// addFileChunk() and removeFileChunks() are the only functions that change
// `ref_count`, and each changes the row and the counter together or not at all.
//
// A DatabaseManager must be used only in the thread that created it
// (one connection per thread, see docs/Architecture.md section 10).
class DatabaseManager {
public:
    explicit DatabaseManager(const QString &vaultRoot);
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager &) = delete;
    DatabaseManager &operator=(const DatabaseManager &) = delete;

    // Creates the folder, the database file and the tables if needed, and
    // turns foreign-key enforcement on. Does nothing if already open.
    Status open();
    void close();
    bool isOpen() const;
    QString databasePath() const;

    // Creates a snapshot row. profileId is optional.
    // The caller should create it inside a transaction.
    Result<std::int64_t> createSnapshot(
        const QString &name,
        bool hasProfileId = false,
        std::int64_t profileId = 0
    );

    // Inserts a file or directory entry and returns its generated ID.
    // entryType must be "file" or "directory".
    Result<std::int64_t> addFileEntry(
        std::int64_t snapshotId,
        const QString &relativePath,
        const QString &entryType,
        std::uint64_t fileSize,
        const QString &contentHash = QString()
    );

    // For callers (e.g. the snapshot code) that must group many changes into
    // one transaction. addFileChunk() and removeFileChunks() work both inside
    // and outside such a transaction.
    Status beginTransaction();
    Status commit();
    Status rollback();

    // Records a chunk that ChunkStorage has just stored, with ref_count 0.
    // If the chunk is already recorded, nothing is changed (deduplication).
    Status addChunk(const QString &hashHex, std::uint64_t originalSize, const StoredChunk &stored);

    Result<bool> chunkExists(const QString &hashHex) const;

    // Fails if the chunk is not in the database.
    Result<ChunkRecord> getChunk(const QString &hashHex) const;

    // Read-only snapshot access for restore operations. Only complete snapshots
    // are returned. File entries are ordered by relative path for stable output.
    Result<SnapshotRecord> getSnapshot(std::int64_t snapshotId) const;
    Result<std::vector<FileEntryRecord>> getSnapshotFiles(
        std::int64_t snapshotId
    ) const;

    // Returns hashes in their original file order (chunk_index 0..N-1).
    // A gap or invalid hash is treated as corrupt snapshot metadata.
    Result<std::vector<QString>> getFileChunkHashes(
        std::int64_t fileId
    ) const;

    // Links a chunk to a file at position chunkIndex and adds 1 to its
    // ref_count. Fails (and changes nothing) if the file or chunk does not
    // exist or the position is already used.
    Status addFileChunk(std::int64_t fileId, int chunkIndex, const QString &hashHex);

    // Removes all chunk links of one file and lowers each chunk's ref_count
    // by the number of links removed. Call this BEFORE deleting a file or
    // snapshot row: deleting the row alone would cascade away the links
    // without lowering the counters.
    Status removeFileChunks(std::int64_t fileId);

private:
    Status openConnection();
    Status createSchemaIfNeeded(QSqlDatabase &db);
    QSqlDatabase database() const;

    QString m_vaultRoot;
    QString m_connectionName;  // empty while closed
};

}  // namespace bv
