#include "core/BackupEngine.h"
#include "core/RestoreEngine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QStringList>

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

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(data) == data.size() && file.flush();
}

QByteArray readFile(const QString &path, bool *ok = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return QByteArray();
    }
    QByteArray data = file.readAll();
    if (ok)
        *ok = file.error() == QFileDevice::NoError;
    return data;
}


bool updateFileSize(DatabaseManager &database, std::int64_t fileId,
                    std::uint64_t fileSize)
{
    const QString connectionName =
        QStringLiteral("restore_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool updated = false;
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        sql.setDatabaseName(database.databasePath());
        if (sql.open()) {
            QSqlQuery query(sql);
            query.prepare("UPDATE files SET file_size = ? WHERE id = ?");
            query.addBindValue(static_cast<qulonglong>(fileSize));
            query.addBindValue(static_cast<qlonglong>(fileId));
            updated = query.exec() && query.numRowsAffected() == 1;
            query.finish();
            sql.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return updated;
}

bool updateChunkStoredSize(DatabaseManager &database, const QString &hash,
                          std::uint64_t storedSize)
{
    const QString connectionName =
        QStringLiteral("restore_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool updated = false;
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        sql.setDatabaseName(database.databasePath());
        if (sql.open()) {
            QSqlQuery query(sql);
            query.prepare("UPDATE chunks SET stored_size = ? WHERE hash = ?");
            query.addBindValue(static_cast<qulonglong>(storedSize));
            query.addBindValue(hash);
            updated = query.exec() && query.numRowsAffected() == 1;
            query.finish();
            sql.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return updated;
}

bool hasRestoreStagingDirectory(const QString &parent)
{
    QDir dir(parent);
    const QStringList entries = dir.entryList(
        QStringList{QStringLiteral(".backupvault-restore-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot
    );
    return !entries.isEmpty();
}

void testRestoreRoundTripAndSafeFailure()
{
    QTemporaryDir temp;
    check(temp.isValid(), "Create restore test temporary directory");
    if (!temp.isValid())
        return;

    const QString source = temp.filePath("source");
    const QString vault = temp.filePath("vault");
    const QString destination = temp.filePath("restored");
    const QString nested = source + "/nested";
    const QString emptyDirectory = nested + "/empty-directory";

    check(QDir().mkpath(emptyDirectory), "Create nested source directories");

    const QByteArray firstChunk(static_cast<int>(kChunkSize), 'A');
    const QByteArray secondChunk(19000, 'B');
    const QByteArray original = firstChunk + secondChunk + firstChunk;
    const bool sourceFilesCreated =
        writeFile(source + "/nested/repeated.bin", original) &&
        writeFile(source + "/empty.txt", QByteArray());
    check(sourceFilesCreated, "Create multi-chunk, repeated-chunk and empty files");
    if (!sourceFilesCreated)
        return;

    DatabaseManager database(vault);
    check(database.open().isOk(), "Open restore test database");
    if (!database.isOpen())
        return;

    ChunkStorage storage(vault);
    ChunkIndex index(database);
    BackupEngine backup(vault, database, storage, index);

    BackupRequest backupRequest;
    backupRequest.sourcePath = source;
    backupRequest.snapshotName = "Restore integration test";
    auto backupResult = backup.createBackup(backupRequest);
    check(backupResult.isOk(), "Create source snapshot through BV-04 BackupEngine");
    if (!backupResult.isOk()) {
        std::cout << "Backup error: " << backupResult.message().toStdString() << '\n';
        return;
    }

    RestoreEngine restore(vault, database, storage);
    RestoreRequest request;
    request.snapshotId = backupResult.value().snapshotId;
    request.destinationPath = destination;

    auto restored = restore.restore(request);
    check(restored.isOk(), "Restore complete BV-04 snapshot");
    if (!restored.isOk()) {
        std::cout << "Restore error: " << restored.message().toStdString() << '\n';
        return;
    }

    bool repeatedReadOk = false;
    bool emptyReadOk = false;
    const QByteArray restoredRepeated =
        readFile(destination + "/nested/repeated.bin", &repeatedReadOk);
    const QByteArray restoredEmpty =
        readFile(destination + "/empty.txt", &emptyReadOk);

    check(repeatedReadOk && restoredRepeated == original,
          "Reconstruct multi-chunk file byte-for-byte in original chunk order");
    check(emptyReadOk && restoredEmpty.isEmpty(),
          "Restore empty file and verify its SHA-256");
    check(QFileInfo(destination + "/nested/empty-directory").isDir(),
          "Restore empty nested directory");
    check(restored.value().filesRestored == 2,
          "Restore summary reports both files");
    check(restored.value().chunksRead == 3,
          "Restore reads repeated chunk occurrences in sequence");
    check(restored.value().bytesRestored == static_cast<std::uint64_t>(original.size()),
          "Restore summary reports exact reconstructed byte count");

    // Existing destinations are never overwritten.
    const QString existingDestination = temp.filePath("already-exists");
    QDir().mkpath(existingDestination);
    const QByteArray marker("keep-existing-data");
    writeFile(existingDestination + "/marker.txt", marker);
    RestoreRequest overwriteRequest;
    overwriteRequest.snapshotId = request.snapshotId;
    overwriteRequest.destinationPath = existingDestination;
    auto overwriteResult = restore.restore(overwriteRequest);
    bool markerReadOk = false;
    const QByteArray markerAfter = readFile(existingDestination + "/marker.txt", &markerReadOk);
    check(!overwriteResult.isOk() && markerReadOk && markerAfter == marker,
          "Reject existing destination without modifying its contents");

    // A raw chunk is limited to kChunkSize bytes plus its 14-byte header.
    // Make both the file and database metadata agree on an oversized value;
    // this ensures RestoreEngine rejects it before ChunkStorage::load() reads it.
    auto entries = database.getSnapshotFiles(request.snapshotId);
    QString repeatedFileId;
    if (entries.isOk()) {
        for (const FileEntryRecord &entry : entries.value()) {
            if (entry.relativePath == QStringLiteral("nested/repeated.bin")) {
                repeatedFileId = QString::number(entry.id);
                break;
            }
        }
    }

    // A file-size mismatch must be detected before the offending chunk is
    // written to the staging file. The test deliberately makes the declared
    // size one byte smaller than the actual reconstructed data.
    bool sizeMismatchPrepared = false;
    if (!repeatedFileId.isEmpty()) {
        const std::int64_t repeatedFileIdValue = repeatedFileId.toLongLong();
        sizeMismatchPrepared = updateFileSize(
            database, repeatedFileIdValue,
            static_cast<std::uint64_t>(original.size() - 1));
        check(sizeMismatchPrepared,
              "Prepare snapshot metadata with a file size smaller than its chunks");
        if (sizeMismatchPrepared) {
            const QString sizeMismatchDestination = temp.filePath("size-mismatch-restore");
            RestoreRequest sizeMismatchRequest;
            sizeMismatchRequest.snapshotId = request.snapshotId;
            sizeMismatchRequest.destinationPath = sizeMismatchDestination;
            auto sizeMismatchResult = restore.restore(sizeMismatchRequest);
            check(!sizeMismatchResult.isOk() &&
                      sizeMismatchResult.message().contains(
                          QStringLiteral("exceed the expected file size")),
                  "Reject a chunk before it can exceed the declared file size");
            check(!QFileInfo::exists(sizeMismatchDestination) &&
                      !hasRestoreStagingDirectory(temp.path()),
                  "Do not publish or retain staging data after file-size mismatch");
            check(updateFileSize(database, repeatedFileIdValue,
                                 static_cast<std::uint64_t>(original.size())),
                  "Restore original file-size metadata after regression test");
        }
    }

    // Corrupt a stored chunk without changing its length. ChunkStorage must
    // reject its SHA-256 mismatch and RestoreEngine must clean the staging tree.
    QString corruptChunkHash;
    if (!repeatedFileId.isEmpty()) {
        auto hashes = database.getFileChunkHashes(repeatedFileId.toLongLong());
        if (hashes.isOk() && !hashes.value().empty())
            corruptChunkHash = hashes.value().front();
    }
    const QString corruptChunkPath = vault + "/chunks/" + corruptChunkHash;
    const QByteArray validChunkFile = readFile(corruptChunkPath);
    QByteArray corruptedChunkFile = validChunkFile;
    if (!corruptedChunkFile.isEmpty())
        corruptedChunkFile[corruptedChunkFile.size() - 1] =
            static_cast<char>(corruptedChunkFile.at(corruptedChunkFile.size() - 1) ^ 0x01);
    const bool corruptChunkPrepared = !corruptChunkHash.isEmpty() &&
        !validChunkFile.isEmpty() && writeFile(corruptChunkPath, corruptedChunkFile);
    check(corruptChunkPrepared, "Prepare same-size chunk-content corruption scenario");
    if (corruptChunkPrepared) {
        const QString corruptDestination = temp.filePath("corrupt-content-restore");
        RestoreRequest corruptContentRequest;
        corruptContentRequest.snapshotId = request.snapshotId;
        corruptContentRequest.destinationPath = corruptDestination;
        auto corruptContentResult = restore.restore(corruptContentRequest);
        check(!corruptContentResult.isOk(),
              "Reject a chunk whose content does not match its SHA-256 name");
        check(!QFileInfo::exists(corruptDestination) &&
                  !hasRestoreStagingDirectory(temp.path()),
              "Clean staging data and avoid publication after chunk hash failure");
        check(writeFile(corruptChunkPath, validChunkFile),
              "Restore original chunk fixture after corruption test");
    }

    // Invalid destination relationships and an active vault lock must fail
    // before any staging directory is created.
    RestoreRequest missingParentRequest;
    missingParentRequest.snapshotId = request.snapshotId;
    missingParentRequest.destinationPath = temp.filePath("missing-parent/restore");
    auto missingParentResult = restore.restore(missingParentRequest);
    check(!missingParentResult.isOk() && !hasRestoreStagingDirectory(temp.path()),
          "Reject a destination whose parent directory does not exist");

    RestoreRequest vaultOverlapRequest;
    vaultOverlapRequest.snapshotId = request.snapshotId;
    vaultOverlapRequest.destinationPath = vault + "/restore-inside-vault";
    auto vaultOverlapResult = restore.restore(vaultOverlapRequest);
    check(!vaultOverlapResult.isOk() && !hasRestoreStagingDirectory(temp.path()),
          "Reject a restore destination inside the backup vault");

    QLockFile competingLock(QDir(vault).filePath("backupvault.lock"));
    competingLock.setStaleLockTime(30000);
    const bool competingLockAcquired = competingLock.tryLock(0);
    check(competingLockAcquired, "Acquire vault lock for lock-conflict regression test");
    if (competingLockAcquired) {
        RestoreRequest lockedRequest;
        lockedRequest.snapshotId = request.snapshotId;
        lockedRequest.destinationPath = temp.filePath("locked-restore");
        auto lockedResult = restore.restore(lockedRequest);
        check(!lockedResult.isOk() &&
                  lockedResult.message().contains(QStringLiteral("vault is locked")),
              "Reject restore while another operation owns the vault lock");
        competingLock.unlock();
    }

    QString oversizedChunkHash;
    if (!repeatedFileId.isEmpty()) {
        auto hashes = database.getFileChunkHashes(repeatedFileId.toLongLong());
        if (hashes.isOk() && !hashes.value().empty())
            oversizedChunkHash = hashes.value().front();
    }

    const QString oversizedChunkPath = vault + "/chunks/" + oversizedChunkHash;
    const QByteArray originalChunkFile = readFile(oversizedChunkPath);
    const std::uint64_t maximumStoredSize =
        static_cast<std::uint64_t>(kChunkSize) + 14;
    const QByteArray oversizedChunkFile(
        static_cast<int>(maximumStoredSize + 1), 'X');

    const bool oversizedChunkReplaced =
        !oversizedChunkHash.isEmpty() && !originalChunkFile.isEmpty() &&
        writeFile(oversizedChunkPath, oversizedChunkFile);
    const bool oversizedMetadataUpdated =
        oversizedChunkReplaced &&
        updateChunkStoredSize(database, oversizedChunkHash, maximumStoredSize + 1);
    const bool oversizedScenarioPrepared =
        oversizedChunkReplaced && oversizedMetadataUpdated;
    check(oversizedScenarioPrepared,
          "Prepare chunk larger than the supported raw storage limit");

    if (oversizedScenarioPrepared) {
        const QString oversizedDestination = temp.filePath("oversized-chunk-restore");
        RestoreRequest oversizedRequest;
        oversizedRequest.snapshotId = request.snapshotId;
        oversizedRequest.destinationPath = oversizedDestination;
        auto oversizedResult = restore.restore(oversizedRequest);
        check(!oversizedResult.isOk() &&
                  oversizedResult.message().contains(
                      QStringLiteral("exceeds the maximum supported raw-chunk size")),
              "Reject oversized stored chunk metadata before loading chunk contents");
        check(!QFileInfo::exists(oversizedDestination),
              "Do not publish output after rejecting an oversized chunk");
    }

    if (oversizedChunkReplaced) {
        const bool restoredOriginalChunk =
            writeFile(oversizedChunkPath, originalChunkFile) &&
            (!oversizedMetadataUpdated ||
             updateChunkStoredSize(database, oversizedChunkHash,
                                   static_cast<std::uint64_t>(originalChunkFile.size())));
        check(restoredOriginalChunk,
              "Restore original chunk fixture after oversized-chunk test");
    }

    // Remove a referenced chunk to simulate missing/corrupt vault data.
    bool missingChunkTestPossible = !repeatedFileId.isEmpty();
    if (missingChunkTestPossible) {
        auto hashes = database.getFileChunkHashes(repeatedFileId.toLongLong());
        missingChunkTestPossible = hashes.isOk() && !hashes.value().empty();
        if (missingChunkTestPossible) {
            const QString missingHash = hashes.value().front();
            missingChunkTestPossible = QFile::remove(vault + "/chunks/" + missingHash);
        }
    }
    check(missingChunkTestPossible, "Prepare missing-chunk corruption scenario");

    const QString failedDestination = temp.filePath("must-not-appear");
    RestoreRequest corruptRequest;
    corruptRequest.snapshotId = request.snapshotId;
    corruptRequest.destinationPath = failedDestination;
    auto corruptResult = restore.restore(corruptRequest);
    check(!corruptResult.isOk(), "Reject restore when a referenced chunk is missing");
    check(!QFileInfo::exists(failedDestination),
          "Do not publish a partially restored destination after failure");
    check(!hasRestoreStagingDirectory(temp.path()),
          "Clean temporary restore data after failure");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testRestoreRoundTripAndSafeFailure();

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
