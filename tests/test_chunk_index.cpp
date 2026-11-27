#include "core/ChunkIndex.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <iostream>

using namespace bv;

int testsPassed = 0;
int testsFailed = 0;

void check(bool condition, const char *testName)
{
    if (condition) {
        std::cout << "[PASS] " << testName << '\n';
        ++testsPassed;
    } else {
        std::cout << "[FAIL] " << testName << '\n';
        ++testsFailed;
    }
}

bool persistChunk(
    DatabaseManager &database,
    ChunkStorage &storage,
    const Bytes &raw,
    ChunkHash &hash
) {
    hash = ChunkIndex::hashBytes(raw);

    const QString hashHex = ChunkIndex::toHex(hash);

    auto stored = storage.store(hashHex, raw);

    if (!stored.isOk()) {
        std::cout << "Storage error: "
                  << stored.message().toStdString() << '\n';
        return false;
    }

    Status status = database.addChunk(
        hashHex,
        static_cast<std::uint64_t>(raw.size()),
        stored.value()
    );

    if (!status.isOk()) {
        std::cout << "Database error: "
                  << status.message().toStdString() << '\n';
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tempDir;

    check(tempDir.isValid(), "Temporary directory creation");

    if (!tempDir.isValid()) {
        return 1;
    }

    DatabaseManager database(tempDir.path());
    Status openStatus = database.open();

    check(openStatus.isOk(), "Database opens");

    if (!openStatus.isOk()) {
        std::cout << openStatus.message().toStdString() << '\n';
        return 1;
    }

    ChunkStorage storage(tempDir.path());

    Bytes rawA = {0x10, 0x20, 0x30, 0x40, 0x50};
    Bytes rawB = {0xAA, 0xBB, 0xCC, 0xDD};

    ChunkHash hashA{};
    ChunkHash hashB{};

    bool storedA = persistChunk(database, storage, rawA, hashA);
    bool storedB = persistChunk(database, storage, rawB, hashB);

    check(storedA, "First chunk stored in database and storage");
    check(storedB, "Second chunk stored in database and storage");

    if (!storedA || !storedB) {
        database.close();
        return 1;
    }

    // One bucket intentionally forces collisions.
    ChunkIndex index(database, 1);

    check(index.size() == 0, "New index starts empty");

    // Test 1: SHA-256 hashing
    {
        Bytes abc = {'a', 'b', 'c'};

        ChunkHash hash = ChunkIndex::hashBytes(abc);

        check(
            ChunkIndex::toHex(hash) ==
                QStringLiteral(
                    "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad"
                ),
            "SHA-256 produces the correct known digest"
        );
    }

    // Test 2: Hexadecimal representation
    {
        QString hex = ChunkIndex::toHex(hashA);

        check(
            hex.size() == 64 &&
            hex == hex.toLower(),
            "Hash converts to 64 lowercase hexadecimal characters"
        );
    }

    // Test 3: Cache miss loads record from SQLite
    {
        auto result = index.find(hashA);

        check(
            result.isOk() &&
            result.value().hash == ChunkIndex::toHex(hashA),
            "Cache miss loads chunk record from SQLite"
        );

        check(
            index.size() == 1,
            "Database lookup populates the cache"
        );
    }

    // Test 4: Cache hit
    {
        auto result = index.find(hashA);

        check(
            result.isOk() && index.size() == 1,
            "Repeated lookup uses existing cache entry"
        );
    }

    // Test 5: Collision handling
    {
        auto result = index.find(hashB);

        check(
            result.isOk() &&
            result.value().hash == ChunkIndex::toHex(hashB),
            "Linked-list chaining handles bucket collisions"
        );

        check(
            index.size() == 2,
            "Both colliding records remain in the table"
        );
    }

    // Test 6: Existence checks
    {
        auto found = index.exists(hashA);
        auto missing = index.exists(ChunkIndex::hashBytes(Bytes{1, 2, 3}));

        check(
            found.isOk() && found.value(),
            "Exists returns true for a stored chunk"
        );

        check(
            missing.isOk() && !missing.value(),
            "Exists returns false for an unknown chunk"
        );
    }

    // Test 7: Duplicate insertion updates instead of duplicating.
    {
        auto record = database.getChunk(ChunkIndex::toHex(hashA));

        bool inserted = record.isOk() &&
                        index.insert(record.value()).isOk();

        check(
            inserted && index.size() == 2,
            "Duplicate insertion does not increase table size"
        );
    }

    // Test 8: Invalid hash rejection
    {
        ChunkRecord invalidRecord;
        invalidRecord.hash = QStringLiteral("invalid");

        Status status = index.insert(invalidRecord);

        check(
            !status.isOk() && index.size() == 2,
            "Invalid hash is rejected"
        );
    }

    // Test 9: Remove only affects the cache.
    {
        bool removed = index.remove(hashA);

        check(
            removed && index.size() == 1,
            "Remove deletes a cached entry"
        );

        check(
            !index.remove(hashA),
            "Removing the same cached entry twice returns false"
        );

        auto result = database.getChunk(ChunkIndex::toHex(hashA));

        check(
            result.isOk(),
            "Removing from cache does not delete the SQLite record"
        );
    }

    // Test 10: Removed record can be loaded again.
    {
        auto result = index.find(hashA);

        check(
            result.isOk() && index.size() == 2,
            "Removed entry can be reloaded from SQLite"
        );
    }

    // Test 11: Clear
    {
        index.clear();

        check(
            index.size() == 0,
            "Clear removes all cached entries"
        );

        check(
            database.getChunk(ChunkIndex::toHex(hashA)).isOk(),
            "Clear does not delete database records"
        );
    }

    // Test 12: Cache can be reused after clear.
    {
        auto result = index.find(hashB);

        check(
            result.isOk() && index.size() == 1,
            "Index works after clear"
        );
    }

    std::cout << "\n========== CHUNK INDEX TEST SUMMARY ==========\n";
    std::cout << "Passed: " << testsPassed << '\n';
    std::cout << "Failed: " << testsFailed << '\n';
    std::cout << "==============================================\n";

    database.close();

    return testsFailed == 0 ? 0 : 1;
}
