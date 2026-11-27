#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QString>
#include <QStringList>
#include <QThread>

#include "common/Result.h"
#include "core/ChunkIndex.h"
#include "core/FileScanner.h"
#include "database/DatabaseManager.h"
#include "storage/ChunkStorage.h"

namespace bv {

struct BackupRequest {
    // Legacy single-folder input. Used when sourcePaths is empty.
    // The folder's contents are stored at the snapshot root.
    QString sourcePath;

    // Multiple file/folder selections. Each selected item is stored under
    // its own base name at the snapshot root. Name collisions are rejected.
    QStringList sourcePaths;

    QString snapshotName;
};

struct BackupProgress {
    QString currentPath;
    std::uint64_t filesCompleted = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t bytesProcessed = 0;
    std::uint64_t totalBytes = 0;
};

using BackupProgressCallback = std::function<void(const BackupProgress &)>;

struct BackupSummary {
    std::int64_t snapshotId = 0;
    std::uint64_t filesBackedUp = 0;
    std::uint64_t directoriesBackedUp = 0;
    std::uint64_t chunksProcessed = 0;
    std::uint64_t originalBytes = 0;
    std::vector<QString> warnings;
};

// Construct this object and all referenced dependencies inside the worker
// thread that will execute createBackup(). The engine is thread-affine.
class BackupEngine {
public:
    BackupEngine(
        const QString &vaultRoot,
        DatabaseManager &database,
        ChunkStorage &storage,
        ChunkIndex &index
    );

    // Callback runs synchronously on the worker thread. GUI callers must
    // forward updates to widgets through a queued signal/connection.
    Result<BackupSummary> createBackup(
        const BackupRequest &request,
        const BackupProgressCallback &progress = {}
    );

private:
    QString m_vaultRoot;
    QThread *m_ownerThread = nullptr;
    DatabaseManager &m_database;
    ChunkStorage &m_storage;
    ChunkIndex &m_index;
    FileScanner m_scanner;
};

} // namespace bv