
#include "core/BackupEngine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <iostream>

using namespace bv;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const char *name)
{
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
        ++passed;
    } else {
        std::cout << "[FAIL] " << name << '\n';
        ++failed;
    }
}

void skip(const char *name)
{
    std::cout << "[SKIP] " << name << '\n';
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly))
        return false;

    return file.write(data) == data.size() && file.flush();
}

bool makeDirectory(const QString &path)
{
    return QDir().mkpath(path);
}

QString hashOf(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            data,
            QCryptographicHash::Sha256
        ).toHex()
    );
}

bool executeSql(const QString &databasePath, const QString &sql)
{
    const QString connectionName =
        "backup_test_" + QUuid::createUuid().toString();

    bool success = false;

    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase("QSQLITE", connectionName);

        db.setDatabaseName(databasePath);

        if (db.open()) {
            QSqlQuery query(db);
            success = query.exec(sql);
            db.close();
        }
    }

    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

QVariant scalar(
    const QString &databasePath,
    const QString &sql
) {
    const QString connectionName =
        "backup_scalar_" + QUuid::createUuid().toString();

    QVariant result;

    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase("QSQLITE", connectionName);

        db.setDatabaseName(databasePath);

        if (db.open()) {
            QSqlQuery query(db);

            if (query.exec(sql) && query.next())
                result = query.value(0);

            db.close();
        }
    }

    QSqlDatabase::removeDatabase(connectionName);
    return result;
}

void testBackupAndDeduplication()
{
    QTemporaryDir temp;

    check(temp.isValid(), "Backup test temporary directory");

    if (!temp.isValid())
        return;

    const QString source = temp.filePath("source");
    const QString vault = temp.filePath("vault");

    makeDirectory(source + "/sub");

    // Two identical full-size chunks in each of two files.
    QByteArray chunk(static_cast<int>(kChunkSize), 'A');
    QByteArray contents = chunk + chunk;

    const bool filesCreated =
        writeFile(source + "/sub/a.bin", contents) &&
        writeFile(source + "/sub/b.bin", contents) &&
        writeFile(source + "/empty.bin", QByteArray());

    check(filesCreated, "Create backup test files");

    if (!filesCreated)
        return;

    DatabaseManager database(vault);

    check(database.open().isOk(), "Open backup database");

    if (!database.isOpen())
        return;

    ChunkStorage storage(vault);
    ChunkIndex index(database);

    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = source;
    request.snapshotName = "Test snapshot";

    auto result = engine.createBackup(request);

    check(result.isOk(), "Create backup successfully");

    if (!result.isOk()) {
        std::cout << "Backup error: "
                  << result.message().toStdString() << '\n';
        return;
    }

    const BackupSummary &summary = result.value();

    check(summary.filesBackedUp == 3,
          "Backup records all three files");

    check(summary.directoriesBackedUp == 2,
          "Backup records root and subdirectory");

    check(summary.chunksProcessed == 4,
          "Backup processes four chunk occurrences");

    check(summary.originalBytes == 4ULL * kChunkSize,
          "Backup reports original file bytes");

    const QString dbPath = database.databasePath();

    check(scalar(dbPath, "SELECT COUNT(*) FROM snapshots").toInt() == 1,
          "One snapshot is persisted");

    check(scalar(dbPath, "SELECT COUNT(*) FROM files").toInt() == 5,
          "Five file and directory entries are persisted");

    check(scalar(dbPath, "SELECT COUNT(*) FROM chunks").toInt() == 1,
          "Identical chunks are deduplicated");

    check(scalar(dbPath, "SELECT COUNT(*) FROM file_chunks").toInt() == 4,
          "All four chunk occurrences are linked");

    check(scalar(dbPath,
                 "SELECT ref_count FROM chunks").toInt() == 4,
          "Chunk reference count matches link count");

    check(scalar(
              dbPath,
              "SELECT content_hash FROM files "
              "WHERE relative_path = 'empty.bin'"
          ).toString() == hashOf(QByteArray()),
          "Empty file has the correct SHA-256 hash");

    check(scalar(
              dbPath,
              "SELECT COUNT(*) FROM files WHERE relative_path = '.'"
          ).toInt() == 1,
          "Source root is stored as relative path '.'");
}

void testVaultExclusion()
{
    QTemporaryDir temp;

    if (!temp.isValid()) {
        check(false, "Vault exclusion temporary directory");
        return;
    }

    const QString source = temp.filePath("source");
    const QString vault = source + "/my-vault";

    makeDirectory(source);
    makeDirectory(vault);

    const bool created =
        writeFile(source + "/keep.txt", QByteArray("keep")) &&
        writeFile(vault + "/should-not-backup.txt",
                  QByteArray("vault data"));

    check(created, "Create vault exclusion test files");

    if (!created)
        return;

    DatabaseManager database(vault);

    if (!database.open().isOk()) {
        check(false, "Open nested vault database");
        return;
    }

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = source;
    request.snapshotName = "Vault exclusion";

    auto result = engine.createBackup(request);

    check(result.isOk(), "Backup source containing nested vault");

    if (!result.isOk()) {
        std::cout << "Vault exclusion error: "
                  << result.message().toStdString() << '\n';
        return;
    }

    check(result.value().filesBackedUp == 1,
          "Only the source file is backed up");

    check(result.value().directoriesBackedUp == 1,
          "Nested vault directory is excluded");

    check(scalar(
              database.databasePath(),
              "SELECT COUNT(*) FROM files "
              "WHERE relative_path LIKE '%my-vault%'"
          ).toInt() == 0,
          "No vault entries are recorded in the snapshot");
}

void testSourceInsideVaultRejected()
{
    QTemporaryDir temp;

    if (!temp.isValid()) {
        check(false, "Source-inside-vault temporary directory");
        return;
    }

    const QString vault = temp.filePath("vault");
    const QString source = vault + "/source";

    makeDirectory(source);
    writeFile(source + "/data.txt", QByteArray("data"));

    DatabaseManager database(vault);

    if (!database.open().isOk()) {
        check(false, "Open database for source-inside-vault test");
        return;
    }

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = source;
    request.snapshotName = "Should be rejected";

    auto result = engine.createBackup(request);

    check(!result.isOk(),
          "Reject source directory located inside vault");

    check(scalar(
              database.databasePath(),
              "SELECT COUNT(*) FROM snapshots"
          ).toInt() == 0,
          "Rejected backup creates no snapshot");
}

void testSourceSymlinkRejected()
{
    QTemporaryDir temp;

    if (!temp.isValid()) {
        check(false, "Source symlink temporary directory");
        return;
    }

    const QString realSource = temp.filePath("real-source");
    const QString linkedSource = temp.filePath("linked-source");
    const QString vault = temp.filePath("vault");

    makeDirectory(realSource);

    if (!writeFile(realSource + "/data.txt", QByteArray("data"))) {
        check(false, "Create source symlink test file");
        return;
    }

    // QFile::link creates a filesystem link where supported.
    if (!QFile::link(realSource, linkedSource) ||
        !QFileInfo(linkedSource).isSymLink()) {
        skip("Source symlink test: filesystem symlinks unavailable");
        return;
    }

    DatabaseManager database(vault);

    if (!database.open().isOk()) {
        check(false, "Open database for source symlink test");
        return;
    }

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = linkedSource;
    request.snapshotName = "Symlink source";

    auto result = engine.createBackup(request);

    check(!result.isOk(),
          "Reject source directory supplied through a symbolic link");

    check(scalar(
              database.databasePath(),
              "SELECT COUNT(*) FROM snapshots"
          ).toInt() == 0,
          "Rejected symlink source creates no snapshot");
}

void testUnreadableSubdirectoryRejected()
{
    QTemporaryDir temp;

    if (!temp.isValid()) {
        check(false, "Unreadable directory temporary directory");
        return;
    }

    const QString source = temp.filePath("source");
    const QString unreadable = source + "/restricted";
    const QString vault = temp.filePath("vault");

    if (!makeDirectory(unreadable) ||
        !writeFile(source + "/normal.txt", QByteArray("normal")) ||
        !writeFile(unreadable + "/hidden.txt", QByteArray("restricted"))) {
        check(false, "Create unreadable directory test files");
        return;
    }

    DatabaseManager database(vault);

    if (!database.open().isOk()) {
        check(false, "Open database for unreadable directory test");
        return;
    }

    // Remove directory read and execute permissions. The backup engine
    // must reject an incomplete scan instead of saving a partial snapshot.
    const QFile::Permissions originalPermissions =
        QFileInfo(unreadable).permissions();

    const bool permissionsChanged = QFile::setPermissions(
        unreadable,
        QFile::Permissions(QFile::WriteOwner)
    );

    if (!permissionsChanged ||
        QFileInfo(unreadable).isReadable()) {
        // Restore permissions before leaving the test.
        QFile::setPermissions(unreadable, originalPermissions);
        skip("Unreadable directory test: permissions cannot be enforced");
        return;
    }

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = source;
    request.snapshotName = "Unreadable directory";

    auto result = engine.createBackup(request);

    // Restore permissions so temporary-directory cleanup can succeed.
    const bool permissionsRestored =
        QFile::setPermissions(unreadable, originalPermissions);

    check(permissionsRestored,
          "Restore unreadable directory permissions");

    check(!result.isOk(),
          "Reject backup containing an unreadable subdirectory");

    check(scalar(
              database.databasePath(),
              "SELECT COUNT(*) FROM snapshots"
          ).toInt() == 0,
          "Unreadable source directory creates no snapshot");
}

void testTransactionRollback()
{
    QTemporaryDir temp;

    if (!temp.isValid()) {
        check(false, "Rollback temporary directory");
        return;
    }

    const QString source = temp.filePath("source");
    const QString vault = temp.filePath("vault");

    makeDirectory(source);

    QByteArray contents("rollback verification data");
    writeFile(source + "/data.bin", contents);

    DatabaseManager database(vault);

    if (!database.open().isOk()) {
        check(false, "Open database for rollback test");
        return;
    }

    // Force a database failure after the chunk and file rows have
    // been created, but before their relationship can be inserted.
    const bool triggerCreated = executeSql(
        database.databasePath(),
        "CREATE TRIGGER reject_test_chunk_link "
        "BEFORE INSERT ON file_chunks "
        "BEGIN "
        "SELECT RAISE(ABORT, 'forced test failure'); "
        "END"
    );

    check(triggerCreated, "Install rollback test trigger");

    if (!triggerCreated)
        return;

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine engine(vault, database, storage, index);

    BackupRequest request;
    request.sourcePath = source;
    request.snapshotName = "Must roll back";

    auto result = engine.createBackup(request);

    check(!result.isOk(),
          "Backup fails when chunk link insertion fails");

    const QString dbPath = database.databasePath();

    check(scalar(dbPath, "SELECT COUNT(*) FROM snapshots").toInt() == 0,
          "Failed backup leaves no snapshot row");

    check(scalar(dbPath, "SELECT COUNT(*) FROM files").toInt() == 0,
          "Failed backup leaves no file rows");

    check(scalar(dbPath, "SELECT COUNT(*) FROM chunks").toInt() == 0,
          "Failed backup leaves no chunk metadata rows");

    check(scalar(dbPath, "SELECT COUNT(*) FROM file_chunks").toInt() == 0,
          "Failed backup leaves no chunk links");

    // Physical chunks written before the transaction rollback are
    // intentionally retained. They are safe orphan data for later GC.
    const QString expectedHash = hashOf(contents);

    check(storage.exists(expectedHash),
          "Physical chunk is retained safely after rollback");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testBackupAndDeduplication();
    testVaultExclusion();
    testSourceInsideVaultRejected();
    testSourceSymlinkRejected();
    testUnreadableSubdirectoryRejected();
    testTransactionRollback();

    std::cout << "\n========== BACKUP ENGINE TEST SUMMARY ==========\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';
    std::cout << "=================================================\n";

    return failed == 0 ? 0 : 1;
}
