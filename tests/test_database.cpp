
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <iostream>

#include "database/DatabaseManager.h"
#include "storage/ChunkStorage.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << std::endl;
        ++failures;
    }
}

static QString hashOf(const Bytes &raw)
{
    QByteArray data(
        reinterpret_cast<const char *>(raw.data()),
        static_cast<int>(raw.size())
    );

    return QString::fromLatin1(
        QCryptographicHash::hash(
            data,
            QCryptographicHash::Sha256
        ).toHex()
    );
}

static Bytes makeBytes(std::size_t size, int seed)
{
    Bytes raw(size);

    for (std::size_t i = 0; i < size; ++i) {
        raw[i] = static_cast<std::uint8_t>(
            (i * 31 + seed) % 256
        );
    }

    return raw;
}

// A second independent connection for direct database inspection.
class RawDb {
public:
    explicit RawDb(const QString &path)
    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase("QSQLITE", "test_raw");

        db.setDatabaseName(path);
        m_ok = db.open();

        if (m_ok)
            run("PRAGMA foreign_keys = ON");
    }

    ~RawDb()
    {
        {
            QSqlDatabase db =
                QSqlDatabase::database("test_raw", false);

            db.close();
        }

        QSqlDatabase::removeDatabase("test_raw");
    }

    bool isOpen() const
    {
        return m_ok;
    }

    bool run(
        const QString &sql,
        const QVariantList &args = QVariantList()
    )
    {
        QSqlQuery query(
            QSqlDatabase::database("test_raw", false)
        );

        query.prepare(sql);

        for (const QVariant &arg : args)
            query.addBindValue(arg);

        return query.exec();
    }

    qlonglong scalar(
        const QString &sql,
        const QVariantList &args = QVariantList()
    )
    {
        QSqlQuery query(
            QSqlDatabase::database("test_raw", false)
        );

        query.prepare(sql);

        for (const QVariant &arg : args)
            query.addBindValue(arg);

        if (!query.exec() || !query.next())
            return -1;

        return query.value(0).toLongLong();
    }

private:
    bool m_ok = false;
};

static qlonglong refCountOf(
    DatabaseManager &db,
    const QString &hash
)
{
    auto chunk = db.getChunk(hash);

    return chunk.isOk()
        ? chunk.value().refCount
        : -1;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    check(dir.isValid(), "temp dir created");

    DatabaseManager db(dir.path());

    check(!db.isOpen(), "not open before open()");

    check(
        !db.addChunk(
            QString(64, 'a'),
            1,
            StoredChunk{"chunks/x", 15}
        ).isOk(),
        "addChunk fails while closed"
    );

    // ---- open / schema ----

    check(db.open().isOk(), "open succeeds");
    check(db.isOpen(), "isOpen after open");
    check(db.open().isOk(), "opening twice is harmless");

    check(
        QFile::exists(db.databasePath()),
        "database file created"
    );

    check(
        db.databasePath().endsWith("storage/backups/vault.db"),
        "database path"
    );

    RawDb raw(db.databasePath());

    check(raw.isOpen(), "raw connection opens");

    for (const char *table : {
             "profiles",
             "snapshots",
             "files",
             "chunks",
             "file_chunks"
         }) {

        check(
            raw.scalar(
                "SELECT COUNT(*) FROM sqlite_master "
                "WHERE type = 'table' AND name = ?",
                {QString(table)}
            ) == 1,
            table
        );
    }

    check(
        raw.scalar("PRAGMA user_version") == 1,
        "schema version saved"
    );

    // ---- test data ----

    check(
        raw.run(
            "INSERT INTO snapshots "
            "(created_at, status) "
            "VALUES ('2026-01-01T00:00:00Z', 'complete')"
        ),
        "insert snapshot"
    );

    check(
        raw.run(
            "INSERT INTO files "
            "(snapshot_id, relative_path, entry_type, file_size) "
            "VALUES (1, 'a.txt', 'file', 10)"
        ),
        "insert file 1"
    );

    check(
        raw.run(
            "INSERT INTO files "
            "(snapshot_id, relative_path, entry_type, file_size) "
            "VALUES (1, 'b.txt', 'file', 20)"
        ),
        "insert file 2"
    );

    // ---- chunk storage ----

    ChunkStorage storage(dir.path());

    Bytes rawA = makeBytes(1000, 7);
    Bytes rawB = makeBytes(kChunkSize, 11);

    QString hashA = hashOf(rawA);
    QString hashB = hashOf(rawB);

    auto storedA = storage.store(hashA, rawA);
    auto storedB = storage.store(hashB, rawB);

    check(
        storedA.isOk() && storedB.isOk(),
        "ChunkStorage stores test chunks"
    );

    // ---- chunk metadata validation ----

    check(
        db.chunkExists(hashA).isOk() &&
        !db.chunkExists(hashA).value(),
        "unknown chunk does not exist"
    );

    check(
        !db.getChunk(hashA).isOk(),
        "getChunk fails for unknown chunk"
    );

    check(
        !db.getChunk("not-a-hash").isOk(),
        "getChunk rejects malformed hash"
    );

    check(
        !db.addChunk("ABCDEF", 1, storedA.value()).isOk(),
        "addChunk rejects malformed hash"
    );

    check(
        !db.addChunk(
            hashA,
            kChunkSize + 1,
            storedA.value()
        ).isOk(),
        "addChunk rejects oversized chunk"
    );

    check(
        db.addChunk(
            hashA,
            rawA.size(),
            storedA.value()
        ).isOk(),
        "addChunk A"
    );

    check(
        db.addChunk(
            hashB,
            rawB.size(),
            storedB.value()
        ).isOk(),
        "addChunk B"
    );

    check(
        db.chunkExists(hashA).isOk() &&
        db.chunkExists(hashA).value(),
        "chunk A exists"
    );

    auto recordA = db.getChunk(hashA);

    check(recordA.isOk(), "getChunk A");

    if (recordA.isOk()) {
        check(
            recordA.value().hash == hashA,
            "record hash"
        );

        check(
            recordA.value().originalSize == rawA.size(),
            "record original size"
        );

        check(
            recordA.value().storedSize ==
                storedA.value().storedSize,
            "record stored size"
        );

        check(
            recordA.value().storagePath ==
                "chunks/" + hashA,
            "record storage path"
        );

        check(
            recordA.value().refCount == 0,
            "new chunk has ref_count 0"
        );

        check(
            !recordA.value().createdAt.isEmpty(),
            "record has created_at"
        );
    }

    // ---- conflicting chunk metadata ----

    StoredChunk wrongSize = storedA.value();
    wrongSize.storedSize += 1;

    check(
        !db.addChunk(
            hashA,
            rawA.size(),
            wrongSize
        ).isOk(),
        "conflicting stored size is rejected"
    );

    StoredChunk wrongPath = storedA.value();
    wrongPath.storagePath = "chunks/another-location";

    check(
        !db.addChunk(
            hashA,
            rawA.size(),
            wrongPath
        ).isOk(),
        "unexpected storage path is rejected"
    );

    check(
        !db.addChunk(
            hashA,
            rawA.size() - 1,
            storedA.value()
        ).isOk(),
        "conflicting original size is rejected"
    );

    auto unchangedA = db.getChunk(hashA);

    check(
        unchangedA.isOk(),
        "original chunk remains available after conflicts"
    );

    if (unchangedA.isOk()) {
        check(
            unchangedA.value().originalSize == rawA.size(),
            "original chunk size remains unchanged"
        );

        check(
            unchangedA.value().storedSize ==
                storedA.value().storedSize,
            "original stored size remains unchanged"
        );

        check(
            unchangedA.value().storagePath ==
                "chunks/" + hashA,
            "original storage path remains unchanged"
        );

        check(
            unchangedA.value().refCount == 0,
            "original reference count remains unchanged"
        );
    }

    // Re-recording matching metadata must remain harmless.
    check(
        db.addChunk(
            hashA,
            rawA.size(),
            storedA.value()
        ).isOk(),
        "adding matching chunk metadata twice succeeds"
    );

    // ---- safe relative path validation ----

    auto rootDirectory = db.addFileEntry(
        1,
        ".",
        "directory",
        0,
        QString()
    );

    check(
        rootDirectory.isOk(),
        "root directory path is accepted"
    );

    auto validNestedDirectory = db.addFileEntry(
        1,
        "folder/subfolder",
        "directory",
        0,
        QString()
    );

    check(
        validNestedDirectory.isOk(),
        "valid nested directory path is accepted"
    );

    auto validFile = db.addFileEntry(
        1,
        "folder/subfolder/valid.txt",
        "file",
        10,
        hashA
    );

    check(
        validFile.isOk(),
        "valid nested file path is accepted"
    );

    check(
        !db.addFileEntry(
            1,
            "../outside.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "parent traversal path is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "folder/../outside.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "nested parent traversal is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "folder//file.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "duplicate path separator is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "folder/./file.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "dot path segment is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "/outside.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "Unix absolute path is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "C:\\outside.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "Windows drive path is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            "folder\\..\\outside.txt",
            "file",
            10,
            hashA
        ).isOk(),
        "Windows parent traversal is rejected"
    );

    check(
        !db.addFileEntry(
            1,
            ".",
            "file",
            10,
            hashA
        ).isOk(),
        "source root cannot be a file"
    );

    check(
        raw.scalar(
            "SELECT COUNT(*) FROM files "
            "WHERE relative_path LIKE '%outside%'"
        ) == 0,
        "unsafe paths were not inserted"
    );

    // ---- reference counts ----

    // State: file 1 -> [A], A = 1, B = 0.
    check(
        db.addFileChunk(1, 0, hashA).isOk(),
        "link A to file 1"
    );

    check(
        db.addChunk(
            hashA,
            rawA.size(),
            storedA.value()
        ).isOk(),
        "addChunk twice is a no-op"
    );

    check(
        refCountOf(db, hashA) == 1,
        "re-adding a chunk keeps its ref_count"
    );

    check(
        db.addFileChunk(1, 1, hashA).isOk(),
        "same chunk twice in one file"
    );

    check(
        refCountOf(db, hashA) == 2,
        "repeated chunk in one file counts twice"
    );

    check(
        db.addFileChunk(2, 0, hashA).isOk(),
        "link A to file 2"
    );

    check(
        db.addFileChunk(2, 1, hashB).isOk(),
        "link B to file 2"
    );

    check(
        refCountOf(db, hashA) == 3,
        "A used by two files"
    );

    check(
        refCountOf(db, hashB) == 1,
        "B used once"
    );

    // Failed links must not change any counter.
    check(
        !db.addFileChunk(2, 1, hashA).isOk(),
        "position already used is rejected"
    );

    check(
        refCountOf(db, hashA) == 3,
        "ref_count unchanged after duplicate position"
    );

    check(
        !db.addFileChunk(999, 0, hashA).isOk(),
        "unknown file is rejected"
    );

    check(
        refCountOf(db, hashA) == 3,
        "ref_count unchanged after unknown file"
    );

    QString unknownHash(64, 'f');

    check(
        !db.addFileChunk(1, 5, unknownHash).isOk(),
        "unknown chunk is rejected"
    );

    check(
        raw.scalar("SELECT COUNT(*) FROM file_chunks") == 4,
        "no stray file_chunks row after failures"
    );

    check(
        !db.addFileChunk(1, -1, hashA).isOk(),
        "negative index is rejected"
    );

    check(
        !db.addFileChunk(1, 9, "xyz").isOk(),
        "malformed hash is rejected"
    );

    check(
        !raw.run(
            "UPDATE chunks SET ref_count = -1 WHERE hash = ?",
            {hashB}
        ),
        "CHECK blocks negative ref_count"
    );

    const QString mismatchSql =
        "SELECT COUNT(*) FROM chunks c WHERE c.ref_count != "
        "(SELECT COUNT(*) FROM file_chunks fc "
        "WHERE fc.chunk_hash = c.hash)";

    check(
        raw.scalar(mismatchSql) == 0,
        "ref_count matches file_chunks"
    );

    // ---- inside a caller's transaction ----

    check(
        db.beginTransaction().isOk(),
        "begin transaction"
    );

    check(
        db.addFileChunk(1, 2, hashB).isOk(),
        "link inside transaction"
    );

    check(
        refCountOf(db, hashB) == 2,
        "counter visible inside transaction"
    );

    check(
        db.rollback().isOk(),
        "rollback"
    );

    check(
        refCountOf(db, hashB) == 1,
        "rollback undoes the counter"
    );

    check(
        raw.scalar(
            "SELECT COUNT(*) FROM file_chunks "
            "WHERE file_id = 1 AND chunk_index = 2"
        ) == 0,
        "rollback undoes the link"
    );

    check(
        db.beginTransaction().isOk(),
        "begin transaction again"
    );

    check(
        db.addFileChunk(1, 2, hashB).isOk(),
        "link inside transaction (commit case)"
    );

    check(
        !db.addFileChunk(1, 2, hashB).isOk(),
        "failed step inside transaction"
    );

    check(
        db.commit().isOk(),
        "commit"
    );

    check(
        refCountOf(db, hashB) == 2,
        "failed step did not break the transaction"
    );

    check(
        raw.scalar(mismatchSql) == 0,
        "ref_count matches file_chunks after transactions"
    );

    // ---- persistence ----

    db.close();

    check(!db.isOpen(), "closed");

    check(
        db.open().isOk(),
        "reopen existing database"
    );

    check(
        refCountOf(db, hashA) == 3 &&
        refCountOf(db, hashB) == 2,
        "ref_counts survive reopening"
    );

    // ---- removing links ----

    // File 1 -> [A, A, B], file 2 -> [A, B].
    check(
        db.removeFileChunks(1).isOk(),
        "remove links of file 1"
    );

    check(
        refCountOf(db, hashA) == 1,
        "A lowered by 2 (appeared twice in file 1)"
    );

    check(
        refCountOf(db, hashB) == 1,
        "B lowered by 1"
    );

    check(
        raw.scalar(
            "SELECT COUNT(*) FROM file_chunks WHERE file_id = 1"
        ) == 0,
        "file 1 links deleted"
    );

    check(
        db.removeFileChunks(1).isOk(),
        "removing again is harmless"
    );

    check(
        refCountOf(db, hashA) == 1,
        "second removal changes nothing"
    );

    check(
        db.removeFileChunks(2).isOk(),
        "remove links of file 2"
    );

    check(
        refCountOf(db, hashA) == 0 &&
        refCountOf(db, hashB) == 0,
        "all counters back to 0"
    );

    check(
        raw.scalar(mismatchSql) == 0,
        "ref_count matches file_chunks at the end"
    );

    if (failures == 0) {
        std::cout << "All database tests passed."
                  << std::endl;
    } else {
        std::cout << failures << " database test(s) failed."
                  << std::endl;
    }

    return failures == 0 ? 0 : 1;
}
