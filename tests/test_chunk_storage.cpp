#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

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
    QByteArray data(reinterpret_cast<const char *>(raw.data()), static_cast<int>(raw.size()));
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

static Bytes makeBytes(std::size_t size)
{
    Bytes raw(size);
    for (std::size_t i = 0; i < size; ++i)
        raw[i] = static_cast<std::uint8_t>((i * 31 + 7) % 256);  // includes 0x00 bytes
    return raw;
}

static void overwrite(const QString &path, const QByteArray &data)
{
    QFile f(path);
    bool opened = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    check(opened, "test helper can open chunk file for writing");
    f.write(data);
}

int main()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir created");
    ChunkStorage storage(dir.path());

    // Round trip: full chunk, small chunk, empty chunk.
    for (std::size_t size : {kChunkSize, std::size_t(1000), std::size_t(1), std::size_t(0)}) {
        Bytes raw = makeBytes(size);
        QString hash = hashOf(raw);
        auto stored = storage.store(hash, raw);
        check(stored.isOk(), "store succeeds");
        check(stored.isOk() && stored.value().storedSize == size + 14, "stored size = header + payload");
        check(stored.isOk() && stored.value().storagePath == "chunks/" + hash, "storage path");
        check(storage.exists(hash), "exists after store");
        auto loaded = storage.load(hash);
        check(loaded.isOk() && loaded.value() == raw, "load returns the same bytes");
    }

    // Storing the same chunk twice is fine.
    Bytes raw = makeBytes(5000);
    QString hash = hashOf(raw);
    check(storage.store(hash, raw).isOk(), "first store");
    check(storage.store(hash, raw).isOk(), "second store (duplicate)");

    // Bad input is rejected.
    check(!storage.store("abc", raw).isOk(), "short hash rejected");
    check(!storage.store(hash.toUpper(), raw).isOk(), "uppercase hash rejected");
    check(!storage.store(hashOf(makeBytes(10)), raw).isOk(), "hash/bytes mismatch rejected");
    check(!storage.store(hashOf(makeBytes(kChunkSize + 1)), makeBytes(kChunkSize + 1)).isOk(), "oversized chunk rejected");
    check(!storage.load("../../etc/passwd").isOk(), "path-like hash rejected");

    // Missing file.
    check(!storage.load(hashOf(makeBytes(77))).isOk(), "missing chunk is an error");
    check(!storage.exists(hashOf(makeBytes(77))), "exists is false for missing chunk");

    // Corruption cases on the stored file.
    QString path = QDir(dir.path()).filePath("chunks/" + hash);
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), "test can open stored chunk file");
    QByteArray good = f.readAll();
    f.close();

    overwrite(path, good.left(5));
    check(!storage.load(hash).isOk(), "truncated header detected");

    overwrite(path, good.left(good.size() - 10));
    check(!storage.load(hash).isOk(), "truncated payload detected");

    QByteArray badMagic = good;
    badMagic[0] = 'X';
    overwrite(path, badMagic);
    check(!storage.load(hash).isOk(), "bad magic detected");

    QByteArray badVersion = good;
    badVersion[4] = 9;
    overwrite(path, badVersion);
    check(!storage.load(hash).isOk(), "unknown version detected");

    QByteArray badMethod = good;
    badMethod[5] = 7;
    overwrite(path, badMethod);
    check(!storage.load(hash).isOk(), "unknown method detected");

    QByteArray flipped = good;
    flipped[20] = static_cast<char>(flipped[20] ^ 0x01);
    overwrite(path, flipped);
    check(!storage.load(hash).isOk(), "flipped byte detected by hash check");

    // A corrupt existing file is repaired by store().
    check(storage.store(hash, raw).isOk(), "store repairs corrupt file");
    auto repaired = storage.load(hash);
    check(repaired.isOk() && repaired.value() == raw, "repaired chunk loads correctly");

    if (failures == 0)
        std::cout << "All ChunkStorage checks passed." << std::endl;
    return failures == 0 ? 0 : 1;
}
