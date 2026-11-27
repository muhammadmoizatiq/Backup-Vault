#pragma once

#include <cstdint>

#include <QString>

#include "common/Result.h"
#include "database/DatabaseManager.h"
#include "storage/ChunkStorage.h"

namespace bv {

struct RestoreRequest {
    std::int64_t snapshotId = 0;
    // The destination directory must not already exist. The engine creates it
    // only after the complete restore has been verified in a staging folder.
    QString destinationPath;
};

struct RestoreSummary {
    std::int64_t snapshotId = 0;
    std::uint64_t filesRestored = 0;
    std::uint64_t directoriesRestored = 0;
    std::uint64_t chunksRead = 0;
    std::uint64_t bytesRestored = 0;
};

// Reconstructs a complete snapshot without modifying any vault data.
class RestoreEngine {
public:
    RestoreEngine(
        const QString &vaultRoot,
        DatabaseManager &database,
        ChunkStorage &storage
    );

    Result<RestoreSummary> restore(const RestoreRequest &request);

private:
    QString m_vaultRoot;
    DatabaseManager &m_database;
    ChunkStorage &m_storage;
};

} // namespace bv
