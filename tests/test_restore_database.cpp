#include <QCoreApplication>
#include <QTemporaryDir>

#include <iostream>

#include "database/DatabaseManager.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::cout << "FAIL: " << message << std::endl;
        ++failures;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory created");
    if (!temporary.isValid())
        return 1;

    DatabaseManager database(temporary.path());
    check(database.open().isOk(), "database opens");

    auto missing = database.getSnapshot(999);
    check(!missing.isOk(), "missing snapshot is rejected");

    auto snapshotResult = database.createSnapshot("restore-test");
    check(snapshotResult.isOk(), "snapshot created");
    if (!snapshotResult.isOk())
        return 1;

    const std::int64_t snapshotId = snapshotResult.value();
    auto snapshot = database.getSnapshot(snapshotId);
    check(snapshot.isOk(), "complete snapshot can be retrieved");
    if (snapshot.isOk()) {
        check(snapshot.value().id == snapshotId, "snapshot ID is preserved");
        check(snapshot.value().name == "restore-test", "snapshot name is preserved");
        check(snapshot.value().status == "complete", "snapshot status is returned");
    }

    auto rootDirectory = database.addFileEntry(
        snapshotId, ".", "directory", 0
    );
    auto nestedDirectory = database.addFileEntry(
        snapshotId, "folder", "directory", 0
    );
    auto file = database.addFileEntry(
        snapshotId,
        "folder/data.bin",
        "file",
        3,
        QString(64, 'c')
    );
    auto emptyFile = database.addFileEntry(
        snapshotId,
        "empty.txt",
        "file",
        0,
        QString(64, 'd')
    );

    check(rootDirectory.isOk(), "root directory entry is accepted");
    check(nestedDirectory.isOk(), "nested directory entry is accepted");
    check(file.isOk(), "file entry is accepted");
    check(emptyFile.isOk(), "empty file entry is accepted");
    if (!rootDirectory.isOk() || !nestedDirectory.isOk() ||
        !file.isOk() || !emptyFile.isOk()) {
        return 1;
    }

    const QString hashA(64, 'a');
    const QString hashB(64, 'b');
    const QString hashC(64, 'e');

    check(database.addChunk(hashA, 1, StoredChunk{"chunks/" + hashA, 15}).isOk(),
          "first chunk metadata is inserted");
    check(database.addChunk(hashB, 1, StoredChunk{"chunks/" + hashB, 15}).isOk(),
          "second chunk metadata is inserted");
    check(database.addChunk(hashC, 1, StoredChunk{"chunks/" + hashC, 15}).isOk(),
          "third chunk metadata is inserted");

    check(database.addFileChunk(file.value(), 0, hashB).isOk(),
          "chunk at index zero is linked");
    check(database.addFileChunk(file.value(), 1, hashA).isOk(),
          "chunk at index one is linked");
    check(database.addFileChunk(file.value(), 2, hashB).isOk(),
          "repeated chunk at index two is linked");

    auto entries = database.getSnapshotFiles(snapshotId);
    check(entries.isOk(), "snapshot entries can be retrieved");
    if (entries.isOk()) {
        check(entries.value().size() == 4, "all file and directory entries are returned");
        bool foundFile = false;
        bool foundRoot = false;
        for (const FileEntryRecord &entry : entries.value()) {
            if (entry.id == file.value()) {
                foundFile = true;
                check(entry.relativePath == "folder/data.bin", "file path is preserved");
                check(entry.fileSize == 3, "file size is preserved");
                check(entry.contentHash == QString(64, 'c'), "file hash is preserved");
            }
            if (entry.id == rootDirectory.value())
                foundRoot = entry.relativePath == "." && entry.entryType == "directory";
        }
        check(foundFile, "requested file is present in snapshot entries");
        check(foundRoot, "root directory is present in snapshot entries");
    }

    auto orderedHashes = database.getFileChunkHashes(file.value());
    check(orderedHashes.isOk(), "file chunk hashes can be retrieved");
    if (orderedHashes.isOk()) {
        check(orderedHashes.value().size() == 3, "all chunk references are returned");
        check(orderedHashes.value()[0] == hashB, "chunk index zero is first");
        check(orderedHashes.value()[1] == hashA, "chunk index one is second");
        check(orderedHashes.value()[2] == hashB, "repeated chunk remains in its original position");
    }

    auto emptyHashes = database.getFileChunkHashes(emptyFile.value());
    check(emptyHashes.isOk(), "empty file chunk list can be retrieved");
    if (emptyHashes.isOk())
        check(emptyHashes.value().empty(), "empty file has no chunk references");

    check(!database.getFileChunkHashes(-1).isOk(), "invalid file ID is rejected");
    check(!database.getSnapshotFiles(-1).isOk(), "invalid snapshot ID is rejected");

    database.close();
    check(!database.getSnapshot(snapshotId).isOk(), "snapshot lookup fails when database is closed");

    if (failures == 0) {
        std::cout << "All restore database tests passed." << std::endl;
        return 0;
    }

    std::cout << failures << " restore database test(s) failed." << std::endl;
    return 1;
}
