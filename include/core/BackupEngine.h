#pragma once

#include <cstdint>
#include <vector>

#include <QString>

#include "common/Result.h"
#include "core/ChunkIndex.h"
#include "core/FileScanner.h"
#include "database/DatabaseManager.h"
#include "storage/ChunkStorage.h"

namespace bv {

struct BackupRequest {
    QString sourcePath;
    QString snapshotName;
};

struct BackupSummary {
    std::int64_t snapshotId = 0;
    std::uint64_t filesBackedUp = 0;
    std::uint64_t directoriesBackedUp = 0;
    std::uint64_t chunksProcessed = 0;
    std::uint64_t originalBytes = 0;
    std::vector<QString> warnings;
};

class BackupEngine {
public:
    BackupEngine(
        const QString &vaultRoot,
        DatabaseManager &database,
        ChunkStorage &storage,
        ChunkIndex &index
    );

    Result<BackupSummary> createBackup(
        const BackupRequest &request
    );

private:
    QString m_vaultRoot;
    DatabaseManager &m_database;
    ChunkStorage &m_storage;
    ChunkIndex &m_index;
    FileScanner m_scanner;
};

} // namespace bv