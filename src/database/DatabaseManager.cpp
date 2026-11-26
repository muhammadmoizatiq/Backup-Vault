#include "database/DatabaseManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace bv {

namespace {

constexpr int kSchemaVersion = 1;

// Same rule as ChunkStorage: 64 lowercase hex characters.
bool isValidHash(const QString &hashHex)
{
    if (hashHex.size() != 64)
        return false;
    for (QChar c : hashHex) {
        bool isDigit = (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
        bool isLowerHex = (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        if (!isDigit && !isLowerHex)
            return false;
    }
    return true;
}

Status notOpen()
{
    return Status::failure("Database is not open.");
}

Status queryFailure(const QString &what, const QSqlQuery &query)
{
    return Status::failure(what + ": " + query.lastError().text());
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

// A savepoint makes "insert the link + raise the counter" all-or-nothing, and
// (unlike BEGIN) it also works when the caller already started a transaction.
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

}  // namespace

DatabaseManager::DatabaseManager(const QString &vaultRoot) : m_vaultRoot(vaultRoot) {}

DatabaseManager::~DatabaseManager()
{
    close();
}

QString DatabaseManager::databasePath() const
{
    return QDir(m_vaultRoot).filePath("storage/backups/vault.db");
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

    if (!QDir().mkpath(QFileInfo(databasePath()).absolutePath()))
        return Status::failure("Cannot create database folder: " + databasePath());

    m_connectionName = "bv_vault_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Status status = openConnection();
    if (!status.isOk())
        close();  // removes the half-opened connection
    return status;
}

// All QSqlDatabase objects live inside this function, so none is still alive
// when close() calls removeDatabase().
Status DatabaseManager::openConnection()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(databasePath());
    if (!db.open())
        return Status::failure("Cannot open database: " + db.lastError().text());

    // SQLite ignores foreign keys unless this is set on every connection.
    QSqlQuery pragma(db);
    if (!pragma.exec("PRAGMA foreign_keys = ON"))
        return queryFailure("Cannot enable foreign keys", pragma);
    if (!pragma.exec("PRAGMA foreign_keys") || !pragma.next() || pragma.value(0).toInt() != 1)
        return Status::failure("SQLite did not enable foreign key enforcement.");
    pragma.finish();

    return createSchemaIfNeeded(db);
}

Status DatabaseManager::createSchemaIfNeeded(QSqlDatabase &db)
{
    QSqlQuery query(db);
    if (!query.exec("PRAGMA user_version") || !query.next())
        return queryFailure("Cannot read database version", query);
    int version = query.value(0).toInt();
    query.finish();

    if (version == kSchemaVersion)
        return Status::success();
    if (version > kSchemaVersion)
        return Status::failure("Database was created by a newer version of BackupVault.");

    // version 0: brand-new file. Create everything or nothing.
    if (!db.transaction())
        return Status::failure("Cannot start schema creation: " + db.lastError().text());
    for (const char *sql : kSchemaStatements) {
        if (!query.exec(QString::fromLatin1(sql))) {
            Status failure = queryFailure("Cannot create database tables", query);
            db.rollback();
            return failure;
        }
    }
    if (!query.exec("PRAGMA user_version = " + QString::number(kSchemaVersion))) {
        Status failure = queryFailure("Cannot save database version", query);
        db.rollback();
        return failure;
    }
    if (!db.commit())
        return Status::failure("Cannot finish schema creation: " + db.lastError().text());
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
    if (!db.transaction())
        return Status::failure("Cannot start transaction: " + db.lastError().text());
    return Status::success();
}

Status DatabaseManager::commit()
{
    if (!isOpen())
        return notOpen();
    QSqlDatabase db = database();
    if (!db.commit())
        return Status::failure("Cannot commit transaction: " + db.lastError().text());
    return Status::success();
}

Status DatabaseManager::rollback()
{
    if (!isOpen())
        return notOpen();
    QSqlDatabase db = database();
    if (!db.rollback())
        return Status::failure("Cannot roll back transaction: " + db.lastError().text());
    return Status::success();
}

Status DatabaseManager::addChunk(const QString &hashHex, std::uint64_t originalSize, const StoredChunk &stored)
{
    if (!isValidHash(hashHex))
        return Status::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (originalSize > kChunkSize)
        return Status::failure("Chunk is larger than the maximum chunk size.");
    if (!isOpen())
        return notOpen();

    // INSERT OR IGNORE keeps the existing row (and its ref_count) if the
    // chunk was already recorded.
    QSqlQuery query(database());
    query.prepare("INSERT OR IGNORE INTO chunks "
                  "(hash, original_size, stored_size, storage_path, ref_count, created_at) "
                  "VALUES (?, ?, ?, ?, 0, ?)");
    query.addBindValue(hashHex);
    query.addBindValue(static_cast<qlonglong>(originalSize));
    query.addBindValue(static_cast<qlonglong>(stored.storedSize));
    query.addBindValue(stored.storagePath);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!query.exec())
        return queryFailure("Cannot record chunk", query);
    return Status::success();
}

Result<bool> DatabaseManager::chunkExists(const QString &hashHex) const
{
    if (!isValidHash(hashHex))
        return Result<bool>::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (!isOpen())
        return Result<bool>::failure("Database is not open.");

    QSqlQuery query(database());
    query.prepare("SELECT 1 FROM chunks WHERE hash = ?");
    query.addBindValue(hashHex);
    if (!query.exec())
        return Result<bool>::failure("Cannot look up chunk: " + query.lastError().text());
    return Result<bool>::success(query.next());
}

Result<ChunkRecord> DatabaseManager::getChunk(const QString &hashHex) const
{
    if (!isValidHash(hashHex))
        return Result<ChunkRecord>::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (!isOpen())
        return Result<ChunkRecord>::failure("Database is not open.");

    QSqlQuery query(database());
    query.prepare("SELECT hash, original_size, stored_size, storage_path, ref_count, created_at "
                  "FROM chunks WHERE hash = ?");
    query.addBindValue(hashHex);
    if (!query.exec())
        return Result<ChunkRecord>::failure("Cannot look up chunk: " + query.lastError().text());
    if (!query.next())
        return Result<ChunkRecord>::failure("Chunk is not in the database: " + hashHex);

    ChunkRecord record;
    record.hash = query.value(0).toString();
    record.originalSize = query.value(1).toULongLong();
    record.storedSize = query.value(2).toULongLong();
    record.storagePath = query.value(3).toString();
    record.refCount = query.value(4).toLongLong();
    record.createdAt = query.value(5).toString();
    return Result<ChunkRecord>::success(record);
}

Status DatabaseManager::addFileChunk(std::int64_t fileId, int chunkIndex, const QString &hashHex)
{
    if (!isValidHash(hashHex))
        return Status::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (chunkIndex < 0)
        return Status::failure("Chunk index cannot be negative.");
    if (!isOpen())
        return notOpen();

    QSqlDatabase db = database();
    Status started = beginSavepoint(db);
    if (!started.isOk())
        return started;

    // The foreign keys reject an unknown file or chunk; the primary key
    // rejects a position that is already used.
    QSqlQuery insert(db);
    insert.prepare("INSERT INTO file_chunks (file_id, chunk_index, chunk_hash) VALUES (?, ?, ?)");
    insert.addBindValue(static_cast<qlonglong>(fileId));
    insert.addBindValue(chunkIndex);
    insert.addBindValue(hashHex);
    if (!insert.exec()) {
        Status failure = queryFailure("Cannot link chunk to file", insert);
        rollbackSavepoint(db);
        return failure;
    }

    QSqlQuery update(db);
    update.prepare("UPDATE chunks SET ref_count = ref_count + 1 WHERE hash = ?");
    update.addBindValue(hashHex);
    if (!update.exec() || update.numRowsAffected() != 1) {
        Status failure = Status::failure("Cannot raise chunk reference count: " + update.lastError().text());
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

    // Lower each affected chunk by the number of links this file has to it
    // (the same chunk can appear several times in one file).
    QSqlQuery update(db);
    update.prepare("UPDATE chunks SET ref_count = ref_count - "
                   "(SELECT COUNT(*) FROM file_chunks "
                   " WHERE file_chunks.file_id = ? AND file_chunks.chunk_hash = chunks.hash) "
                   "WHERE hash IN (SELECT chunk_hash FROM file_chunks WHERE file_id = ?)");
    update.addBindValue(static_cast<qlonglong>(fileId));
    update.addBindValue(static_cast<qlonglong>(fileId));
    if (!update.exec()) {
        Status failure = queryFailure("Cannot lower chunk reference counts", update);
        rollbackSavepoint(db);
        return failure;
    }

    QSqlQuery remove(db);
    remove.prepare("DELETE FROM file_chunks WHERE file_id = ?");
    remove.addBindValue(static_cast<qlonglong>(fileId));
    if (!remove.exec()) {
        Status failure = queryFailure("Cannot remove chunk links", remove);
        rollbackSavepoint(db);
        return failure;
    }

    return releaseSavepoint(db);
}

}  // namespace bv
