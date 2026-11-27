
#include "database/DatabaseManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <limits>
#include <utility>

namespace bv {

namespace {

constexpr int kSchemaVersion = 1;

// Same rule as ChunkStorage: 64 lowercase hexadecimal characters.
bool isValidHash(const QString &hashHex)
{
    if (hashHex.size() != 64)
        return false;

    for (QChar c : hashHex) {
        const bool isDigit =
            c >= QLatin1Char('0') && c <= QLatin1Char('9');

        const bool isLowerHex =
            c >= QLatin1Char('a') && c <= QLatin1Char('f');

        if (!isDigit && !isLowerHex)
            return false;
    }

    return true;
}

// Normalize and validate a relative path before storing it.
//
// "." represents the source root directory.
// Empty segments, "." segments, ".." segments, absolute paths,
// drive-qualified paths, and null characters are rejected.
bool normalizeSafeRelativePath(QString &path)
{
    path.replace('\\', '/');

    if (path == QStringLiteral("."))
        return true;

    if (path.isEmpty() ||
        path.startsWith('/') ||
        path.contains(':') ||
        path.contains(QChar(0)) ||
        QDir::isAbsolutePath(path)) {
        return false;
    }

    const QStringList segments =
        path.split('/', Qt::KeepEmptyParts);

    for (const QString &segment : segments) {
        if (segment.isEmpty() ||
            segment == QStringLiteral(".") ||
            segment == QStringLiteral("..")) {
            return false;
        }
    }

    return true;
}

Status notOpen()
{
    return Status::failure("Database is not open.");
}

Status queryFailure(const QString &what, const QSqlQuery &query)
{
    return Status::failure(
        what + ": " + query.lastError().text()
    );
}

// Same tables, columns and indexes as docs/DatabaseSchema.md.
const char *const kSchemaStatements[] = {
    "CREATE TABLE IF NOT EXISTS profiles ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  source_path TEXT NOT NULL,"
    "  exclude_rules TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL)",

    "CREATE TABLE IF NOT EXISTS snapshots ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  profile_id INTEGER,"
    "  name TEXT,"
    "  created_at TEXT NOT NULL,"
    "  status TEXT NOT NULL,"
    "  FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE SET NULL)",

    "CREATE TABLE IF NOT EXISTS files ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  snapshot_id INTEGER NOT NULL,"
    "  relative_path TEXT NOT NULL,"
    "  entry_type TEXT NOT NULL,"
    "  file_size INTEGER NOT NULL DEFAULT 0,"
    "  content_hash TEXT,"
    "  UNIQUE (snapshot_id, relative_path),"
    "  FOREIGN KEY (snapshot_id) REFERENCES snapshots(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS chunks ("
    "  hash TEXT PRIMARY KEY,"
    "  original_size INTEGER NOT NULL,"
    "  stored_size INTEGER NOT NULL,"
    "  storage_path TEXT NOT NULL,"
    "  ref_count INTEGER NOT NULL DEFAULT 0 CHECK (ref_count >= 0),"
    "  created_at TEXT NOT NULL)",

    "CREATE TABLE IF NOT EXISTS file_chunks ("
    "  file_id INTEGER NOT NULL,"
    "  chunk_index INTEGER NOT NULL,"
    "  chunk_hash TEXT NOT NULL,"
    "  PRIMARY KEY (file_id, chunk_index),"
    "  FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (chunk_hash) REFERENCES chunks(hash))",

    "CREATE INDEX IF NOT EXISTS idx_snapshots_profile_id ON snapshots(profile_id)",
    "CREATE INDEX IF NOT EXISTS idx_files_snapshot_id ON files(snapshot_id)",
    "CREATE INDEX IF NOT EXISTS idx_file_chunks_chunk_hash ON file_chunks(chunk_hash)",
};

// A savepoint makes "insert the link + raise the counter" all-or-nothing.
// It also works when the caller already started a transaction.
Status beginSavepoint(QSqlDatabase &db)
{
    QSqlQuery query(db);

    if (!query.exec("SAVEPOINT bv_step"))
        return queryFailure("Cannot start database step", query);

    return Status::success();
}

Status releaseSavepoint(QSqlDatabase &db)
{
    QSqlQuery query(db);

    if (!query.exec("RELEASE bv_step"))
        return queryFailure("Cannot finish database step", query);

    return Status::success();
}

void rollbackSavepoint(QSqlDatabase &db)
{
    QSqlQuery query(db);
    query.exec("ROLLBACK TO bv_step");
    query.exec("RELEASE bv_step");
}

} // namespace

DatabaseManager::DatabaseManager(const QString &vaultRoot)
    : m_vaultRoot(vaultRoot)
{
}

DatabaseManager::~DatabaseManager()
{
    close();
}

QString DatabaseManager::databasePath() const
{
    return QDir(m_vaultRoot).filePath(
        "storage/backups/vault.db"
    );
}

QSqlDatabase DatabaseManager::database() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

bool DatabaseManager::isOpen() const
{
    return !m_connectionName.isEmpty() && database().isOpen();
}

Status DatabaseManager::open()
{
    if (isOpen())
        return Status::success();

    if (!QDir().mkpath(QFileInfo(databasePath()).absolutePath())) {
        return Status::failure(
            "Cannot create database folder: " + databasePath()
        );
    }

    m_connectionName =
        "bv_vault_" +
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    Status status = openConnection();

    if (!status.isOk())
        close();

    return status;
}

// All QSqlDatabase objects live inside this function, so none remains
// alive when close() calls removeDatabase().
Status DatabaseManager::openConnection()
{
    QSqlDatabase db =
        QSqlDatabase::addDatabase("QSQLITE", m_connectionName);

    db.setDatabaseName(databasePath());

    if (!db.open()) {
        return Status::failure(
            "Cannot open database: " + db.lastError().text()
        );
    }

    // SQLite ignores foreign keys unless enabled on every connection.
    QSqlQuery pragma(db);

    if (!pragma.exec("PRAGMA foreign_keys = ON"))
        return queryFailure("Cannot enable foreign keys", pragma);

    if (!pragma.exec("PRAGMA foreign_keys") ||
        !pragma.next() ||
        pragma.value(0).toInt() != 1) {
        return Status::failure(
            "SQLite did not enable foreign key enforcement."
        );
    }

    pragma.finish();

    return createSchemaIfNeeded(db);
}

Status DatabaseManager::createSchemaIfNeeded(QSqlDatabase &db)
{
    QSqlQuery query(db);

    if (!query.exec("PRAGMA user_version") || !query.next()) {
        return queryFailure(
            "Cannot read database version",
            query
        );
    }

    const int version = query.value(0).toInt();
    query.finish();

    if (version == kSchemaVersion)
        return Status::success();

    if (version > kSchemaVersion) {
        return Status::failure(
            "Database was created by a newer version of BackupVault."
        );
    }

    // Version 0: create the complete schema transactionally.
    if (!db.transaction()) {
        return Status::failure(
            "Cannot start schema creation: " + db.lastError().text()
        );
    }

    for (const char *sql : kSchemaStatements) {
        if (!query.exec(QString::fromLatin1(sql))) {
            Status failure =
                queryFailure("Cannot create database tables", query);

            db.rollback();
            return failure;
        }
    }

    if (!query.exec(
            "PRAGMA user_version = " +
            QString::number(kSchemaVersion))) {
        Status failure =
            queryFailure("Cannot save database version", query);

        db.rollback();
        return failure;
    }

    if (!db.commit()) {
        return Status::failure(
            "Cannot finish schema creation: " +
            db.lastError().text()
        );
    }

    return Status::success();
}

void DatabaseManager::close()
{
    if (m_connectionName.isEmpty())
        return;

    {
        QSqlDatabase db = database();

        if (db.isOpen())
            db.close();
    }

    QSqlDatabase::removeDatabase(m_connectionName);
    m_connectionName.clear();
}

Status DatabaseManager::beginTransaction()
{
    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();

    if (!db.transaction()) {
        return Status::failure(
            "Cannot start transaction: " + db.lastError().text()
        );
    }

    return Status::success();
}

Status DatabaseManager::commit()
{
    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();

    if (!db.commit()) {
        return Status::failure(
            "Cannot commit transaction: " + db.lastError().text()
        );
    }

    return Status::success();
}

Status DatabaseManager::rollback()
{
    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();

    if (!db.rollback()) {
        return Status::failure(
            "Cannot roll back transaction: " + db.lastError().text()
        );
    }

    return Status::success();
}

Status DatabaseManager::addChunk(
    const QString &hashHex,
    std::uint64_t originalSize,
    const StoredChunk &stored
)
{
    if (!isValidHash(hashHex)) {
        return Status::failure(
            "Invalid chunk hash (need 64 lowercase hex characters)."
        );
    }

    if (originalSize > kChunkSize) {
        return Status::failure(
            "Chunk is larger than the maximum chunk size."
        );
    }

    if (!isOpen())
        return notOpen();

    if (stored.storedSize == 0 ||
        stored.storedSize > static_cast<std::uint64_t>(
            std::numeric_limits<qlonglong>::max())) {
        return Status::failure(
            "Stored chunk size is outside the supported database range."
        );
    }

    const QString expectedStoragePath =
        QStringLiteral("chunks/") + hashHex;

    if (stored.storagePath != expectedStoragePath) {
        return Status::failure(
            "Stored chunk has an unexpected storage path."
        );
    }

    QSqlQuery query(database());

    query.prepare(
        "INSERT OR IGNORE INTO chunks "
        "(hash, original_size, stored_size, storage_path, ref_count, created_at) "
        "VALUES (?, ?, ?, ?, 0, ?)"
    );

    query.addBindValue(hashHex);
    query.addBindValue(static_cast<qlonglong>(originalSize));
    query.addBindValue(static_cast<qlonglong>(stored.storedSize));
    query.addBindValue(stored.storagePath);

    query.addBindValue(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
    );

    if (!query.exec())
        return queryFailure("Cannot record chunk", query);

    query.finish();

    // INSERT OR IGNORE may have found an existing row. Verify that its
    // metadata matches the chunk validated by ChunkStorage.
    QSqlQuery verify(database());

    verify.prepare(
        "SELECT original_size, stored_size, storage_path "
        "FROM chunks WHERE hash = ?"
    );

    verify.addBindValue(hashHex);

    if (!verify.exec()) {
        return queryFailure(
            "Cannot verify existing chunk metadata",
            verify
        );
    }

    if (!verify.next()) {
        return Status::failure(
            "Chunk row is missing after insertion: " + hashHex
        );
    }

    const std::uint64_t recordedOriginalSize =
        verify.value(0).toULongLong();

    const std::uint64_t recordedStoredSize =
        verify.value(1).toULongLong();

    const QString recordedStoragePath =
        verify.value(2).toString();

    if (recordedOriginalSize != originalSize ||
        recordedStoredSize != stored.storedSize ||
        recordedStoragePath != stored.storagePath) {
        return Status::failure(
            "Existing chunk metadata does not match the stored chunk: " +
            hashHex
        );
    }

    return Status::success();
}

Result<bool> DatabaseManager::chunkExists(
    const QString &hashHex
) const
{
    if (!isValidHash(hashHex)) {
        return Result<bool>::failure(
            "Invalid chunk hash (need 64 lowercase hex characters)."
        );
    }

    if (!isOpen()) {
        return Result<bool>::failure(
            "Database is not open."
        );
    }

    QSqlQuery query(database());

    query.prepare(
        "SELECT 1 FROM chunks WHERE hash = ?"
    );

    query.addBindValue(hashHex);

    if (!query.exec()) {
        return Result<bool>::failure(
            "Cannot look up chunk: " +
            query.lastError().text()
        );
    }

    return Result<bool>::success(query.next());
}

Result<ChunkRecord> DatabaseManager::getChunk(
    const QString &hashHex
) const
{
    if (!isValidHash(hashHex)) {
        return Result<ChunkRecord>::failure(
            "Invalid chunk hash (need 64 lowercase hex characters)."
        );
    }

    if (!isOpen()) {
        return Result<ChunkRecord>::failure(
            "Database is not open."
        );
    }

    QSqlQuery query(database());

    query.prepare(
        "SELECT hash, original_size, stored_size, storage_path, "
        "ref_count, created_at "
        "FROM chunks WHERE hash = ?"
    );

    query.addBindValue(hashHex);

    if (!query.exec()) {
        return Result<ChunkRecord>::failure(
            "Cannot look up chunk: " +
            query.lastError().text()
        );
    }

    if (!query.next()) {
        return Result<ChunkRecord>::failure(
            "Chunk is not in the database: " + hashHex
        );
    }

    ChunkRecord record;

    record.hash = query.value(0).toString();
    record.originalSize = query.value(1).toULongLong();
    record.storedSize = query.value(2).toULongLong();
    record.storagePath = query.value(3).toString();
    record.refCount = query.value(4).toLongLong();
    record.createdAt = query.value(5).toString();

    return Result<ChunkRecord>::success(record);
}

Result<SnapshotRecord> DatabaseManager::getSnapshot(
    std::int64_t snapshotId
) const
{
    if (snapshotId <= 0) {
        return Result<SnapshotRecord>::failure("Invalid snapshot ID.");
    }

    if (!isOpen()) {
        return Result<SnapshotRecord>::failure("Database is not open.");
    }

    QSqlQuery query(database());
    query.prepare(
        "SELECT id, name, status FROM snapshots WHERE id = ?"
    );
    query.addBindValue(static_cast<qlonglong>(snapshotId));

    if (!query.exec()) {
        return Result<SnapshotRecord>::failure(
            "Cannot retrieve snapshot: " + query.lastError().text()
        );
    }

    if (!query.next()) {
        return Result<SnapshotRecord>::failure("Snapshot does not exist.");
    }

    SnapshotRecord snapshot;
    snapshot.id = query.value(0).toLongLong();
    snapshot.name = query.value(1).toString();
    snapshot.status = query.value(2).toString();

    if (snapshot.status != QStringLiteral("complete")) {
        return Result<SnapshotRecord>::failure(
            "Snapshot is not complete and cannot be restored."
        );
    }

    return Result<SnapshotRecord>::success(snapshot);
}

Result<std::vector<FileEntryRecord>> DatabaseManager::getSnapshotFiles(
    std::int64_t snapshotId
) const
{
    if (snapshotId <= 0) {
        return Result<std::vector<FileEntryRecord>>::failure(
            "Invalid snapshot ID."
        );
    }

    if (!isOpen()) {
        return Result<std::vector<FileEntryRecord>>::failure(
            "Database is not open."
        );
    }

    // Do not expose entries from missing or incomplete snapshots.
    auto snapshot = getSnapshot(snapshotId);
    if (!snapshot.isOk()) {
        return Result<std::vector<FileEntryRecord>>::failure(
            snapshot.message()
        );
    }

    QSqlQuery query(database());
    query.prepare(
        "SELECT id, snapshot_id, relative_path, entry_type, file_size, content_hash "
        "FROM files WHERE snapshot_id = ? ORDER BY relative_path COLLATE BINARY"
    );
    query.addBindValue(static_cast<qlonglong>(snapshotId));

    if (!query.exec()) {
        return Result<std::vector<FileEntryRecord>>::failure(
            "Cannot retrieve snapshot file entries: " + query.lastError().text()
        );
    }

    std::vector<FileEntryRecord> entries;
    while (query.next()) {
        FileEntryRecord entry;
        entry.id = query.value(0).toLongLong();
        entry.snapshotId = query.value(1).toLongLong();
        entry.relativePath = query.value(2).toString();
        entry.entryType = query.value(3).toString();
        bool fileSizeOk = false;
        const qlonglong storedFileSize = query.value(4).toLongLong(&fileSizeOk);
        entry.contentHash = query.value(5).toString();

        if (entry.id <= 0 || entry.snapshotId != snapshotId ||
            !fileSizeOk || storedFileSize < 0 ||
            entry.relativePath.isEmpty() ||
            (entry.entryType != QStringLiteral("file") &&
             entry.entryType != QStringLiteral("directory"))) {
            return Result<std::vector<FileEntryRecord>>::failure(
                "Snapshot contains invalid file metadata."
            );
        }
        entry.fileSize = static_cast<std::uint64_t>(storedFileSize);

        QString checkedPath = entry.relativePath;
        if (!normalizeSafeRelativePath(checkedPath) ||
            checkedPath != entry.relativePath) {
            return Result<std::vector<FileEntryRecord>>::failure(
                "Snapshot contains an unsafe relative path."
            );
        }

        if (entry.entryType == QStringLiteral("directory")) {
            if (entry.fileSize != 0 || !entry.contentHash.isEmpty()) {
                return Result<std::vector<FileEntryRecord>>::failure(
                    "Snapshot contains invalid directory metadata."
                );
            }
        } else if (!isValidHash(entry.contentHash)) {
            return Result<std::vector<FileEntryRecord>>::failure(
                "Snapshot contains an invalid file content hash."
            );
        }

        entries.push_back(std::move(entry));
    }

    return Result<std::vector<FileEntryRecord>>::success(std::move(entries));
}

Result<std::vector<QString>> DatabaseManager::getFileChunkHashes(
    std::int64_t fileId
) const
{
    if (fileId <= 0) {
        return Result<std::vector<QString>>::failure("Invalid file ID.");
    }

    if (!isOpen()) {
        return Result<std::vector<QString>>::failure("Database is not open.");
    }

    QSqlQuery query(database());
    query.prepare(
        "SELECT chunk_index, chunk_hash FROM file_chunks "
        "WHERE file_id = ? ORDER BY chunk_index ASC"
    );
    query.addBindValue(static_cast<qlonglong>(fileId));

    if (!query.exec()) {
        return Result<std::vector<QString>>::failure(
            "Cannot retrieve file chunk order: " + query.lastError().text()
        );
    }

    std::vector<QString> hashes;
    int expectedIndex = 0;

    while (query.next()) {
        bool indexOk = false;
        const qlonglong actualIndex = query.value(0).toLongLong(&indexOk);
        const QString hash = query.value(1).toString();

        if (!indexOk || actualIndex != expectedIndex || !isValidHash(hash)) {
            return Result<std::vector<QString>>::failure(
                "File has invalid or non-contiguous chunk ordering."
            );
        }

        hashes.push_back(hash);
        if (expectedIndex == std::numeric_limits<int>::max()) {
            return Result<std::vector<QString>>::failure(
                "File contains too many chunk references."
            );
        }
        ++expectedIndex;
    }

    return Result<std::vector<QString>>::success(std::move(hashes));
}

Status DatabaseManager::addFileChunk(
    std::int64_t fileId,
    int chunkIndex,
    const QString &hashHex
)
{
    if (!isValidHash(hashHex)) {
        return Status::failure(
            "Invalid chunk hash (need 64 lowercase hex characters)."
        );
    }

    if (chunkIndex < 0) {
        return Status::failure(
            "Chunk index cannot be negative."
        );
    }

    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();

    Status started = beginSavepoint(db);

    if (!started.isOk())
        return started;

    // Foreign keys reject unknown files/chunks; the primary key rejects
    // a chunk position that is already used.
    QSqlQuery insert(db);

    insert.prepare(
        "INSERT INTO file_chunks "
        "(file_id, chunk_index, chunk_hash) "
        "VALUES (?, ?, ?)"
    );

    insert.addBindValue(static_cast<qlonglong>(fileId));
    insert.addBindValue(chunkIndex);
    insert.addBindValue(hashHex);

    if (!insert.exec()) {
        Status failure =
            queryFailure("Cannot link chunk to file", insert);

        rollbackSavepoint(db);
        return failure;
    }

    QSqlQuery update(db);

    update.prepare(
        "UPDATE chunks SET ref_count = ref_count + 1 WHERE hash = ?"
    );

    update.addBindValue(hashHex);

    if (!update.exec() || update.numRowsAffected() != 1) {
        Status failure = Status::failure(
            "Cannot raise chunk reference count: " +
            update.lastError().text()
        );

        rollbackSavepoint(db);
        return failure;
    }

    return releaseSavepoint(db);
}

Status DatabaseManager::removeFileChunks(std::int64_t fileId)
{
    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();

    Status started = beginSavepoint(db);

    if (!started.isOk())
        return started;

    // Lower each affected chunk by the number of links this file has to it.
    // The same chunk can appear several times in one file.
    QSqlQuery update(db);

    update.prepare(
        "UPDATE chunks SET ref_count = ref_count - "
        "(SELECT COUNT(*) FROM file_chunks "
        " WHERE file_chunks.file_id = ? "
        " AND file_chunks.chunk_hash = chunks.hash) "
        "WHERE hash IN "
        "(SELECT chunk_hash FROM file_chunks WHERE file_id = ?)"
    );

    update.addBindValue(static_cast<qlonglong>(fileId));
    update.addBindValue(static_cast<qlonglong>(fileId));

    if (!update.exec()) {
        Status failure =
            queryFailure("Cannot lower chunk reference counts", update);

        rollbackSavepoint(db);
        return failure;
    }

    QSqlQuery remove(db);

    remove.prepare(
        "DELETE FROM file_chunks WHERE file_id = ?"
    );

    remove.addBindValue(static_cast<qlonglong>(fileId));

    if (!remove.exec()) {
        Status failure =
            queryFailure("Cannot remove chunk links", remove);

        rollbackSavepoint(db);
        return failure;
    }

    return releaseSavepoint(db);
}

Result<std::int64_t> DatabaseManager::createSnapshot(
    const QString &name,
    bool hasProfileId,
    std::int64_t profileId
)
{
    if (!isOpen()) {
        return Result<std::int64_t>::failure(
            "Database is not open."
        );
    }

    if (hasProfileId && profileId <= 0) {
        return Result<std::int64_t>::failure(
            "Profile ID must be positive."
        );
    }

    QSqlQuery query(database());

    query.prepare(
        "INSERT INTO snapshots (profile_id, name, created_at, status) "
        "VALUES (?, ?, ?, ?)"
    );

    if (hasProfileId)
        query.addBindValue(static_cast<qlonglong>(profileId));
    else
        query.addBindValue(QVariant());

    if (name.isEmpty())
        query.addBindValue(QVariant());
    else
        query.addBindValue(name);

    query.addBindValue(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
    );

    // The row is not visible outside the transaction until committed.
    query.addBindValue(QStringLiteral("complete"));

    if (!query.exec()) {
        return Result<std::int64_t>::failure(
            "Cannot create snapshot: " +
            query.lastError().text()
        );
    }

    bool validId = false;

    const qlonglong id =
        query.lastInsertId().toLongLong(&validId);

    if (!validId || id <= 0) {
        return Result<std::int64_t>::failure(
            "Could not obtain the new snapshot ID."
        );
    }

    return Result<std::int64_t>::success(
        static_cast<std::int64_t>(id)
    );
}

Result<std::int64_t> DatabaseManager::addFileEntry(
    std::int64_t snapshotId,
    const QString &relativePath,
    const QString &entryType,
    std::uint64_t fileSize,
    const QString &contentHash
)
{
    if (!isOpen()) {
        return Result<std::int64_t>::failure(
            "Database is not open."
        );
    }

    if (snapshotId <= 0) {
        return Result<std::int64_t>::failure(
            "Invalid snapshot ID."
        );
    }

    QString safeRelativePath = relativePath;

    if (!normalizeSafeRelativePath(safeRelativePath)) {
        return Result<std::int64_t>::failure(
            "File path must be a safe relative path."
        );
    }

    const bool isFile =
        entryType == QStringLiteral("file");

    const bool isDirectory =
        entryType == QStringLiteral("directory");

    if (!isFile && !isDirectory) {
        return Result<std::int64_t>::failure(
            "Entry type must be 'file' or 'directory'."
        );
    }

    if (isFile && safeRelativePath == QStringLiteral(".")) {
        return Result<std::int64_t>::failure(
            "The source root path must be a directory."
        );
    }

    if (fileSize > static_cast<std::uint64_t>(
            std::numeric_limits<qlonglong>::max())) {
        return Result<std::int64_t>::failure(
            "File size exceeds the supported database range."
        );
    }

    if (isDirectory &&
        (fileSize != 0 || !contentHash.isEmpty())) {
        return Result<std::int64_t>::failure(
            "Directory entries must have size zero and no content hash."
        );
    }

    if (isFile && !isValidHash(contentHash)) {
        return Result<std::int64_t>::failure(
            "File entries require a valid SHA-256 content hash."
        );
    }

    QSqlQuery query(database());

    query.prepare(
        "INSERT INTO files "
        "(snapshot_id, relative_path, entry_type, file_size, content_hash) "
        "VALUES (?, ?, ?, ?, ?)"
    );

    query.addBindValue(static_cast<qlonglong>(snapshotId));
    query.addBindValue(safeRelativePath);
    query.addBindValue(entryType);
    query.addBindValue(static_cast<qlonglong>(fileSize));

    if (isDirectory)
        query.addBindValue(QVariant());
    else
        query.addBindValue(contentHash);

    if (!query.exec()) {
        return Result<std::int64_t>::failure(
            "Cannot insert file entry: " +
            query.lastError().text()
        );
    }

    bool validId = false;

    const qlonglong id =
        query.lastInsertId().toLongLong(&validId);

    if (!validId || id <= 0) {
        return Result<std::int64_t>::failure(
            "Could not obtain the new file ID."
        );
    }

    return Result<std::int64_t>::success(
        static_cast<std::int64_t>(id)
    );
}

} // namespace bv
