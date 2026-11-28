
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    QByteArray data(reinterpret_cast<const char *>(raw.data()),
                    static_cast<int>(raw.size()));
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

static Bytes makeBytes(std::size_t size)
{
    Bytes raw(size);
    for (std::size_t i = 0; i < size; ++i)
        raw[i] = static_cast<std::uint8_t>((i * 31 + 7) % 256);
    return raw;
}

static bool overwrite(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size() && file.flush();
}

int main()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir created");
    if (!dir.isValid())
        return 1;

    ChunkStorage storage(dir.path());

    // Round trip: full chunk, small chunk, one-byte chunk and empty chunk.
    for (std::size_t size : {
             kChunkSize, std::size_t(1000), std::size_t(1), std::size_t(0)}) {
        const Bytes raw = makeBytes(size);
        const QString hash = hashOf(raw);
        const auto stored = storage.store(hash, raw);

        check(stored.isOk(), "store succeeds");
        check(stored.isOk() && stored.value().storedSize == size + 14,
              "stored size = header + payload");
        check(stored.isOk() && stored.value().storagePath == "chunks/" + hash,
              "storage path");
        check(storage.exists(hash), "exists after store");

        const auto loaded = storage.load(hash);
        check(loaded.isOk() && loaded.value() == raw,
              "load returns the same bytes");
    }

    // Storing the same chunk twice is fine.
    const Bytes raw = makeBytes(5000);
    const QString hash = hashOf(raw);
    check(storage.store(hash, raw).isOk(), "first store");
    check(storage.store(hash, raw).isOk(), "second store (duplicate)");

    // Bad input is rejected.
    check(!storage.store("abc", raw).isOk(), "short hash rejected");
    check(!storage.store(hash.toUpper(), raw).isOk(),
          "uppercase hash rejected");
    check(!storage.store(hashOf(makeBytes(10)), raw).isOk(),
          "hash/bytes mismatch rejected");
    check(!storage.store(hashOf(makeBytes(kChunkSize + 1)),
                         makeBytes(kChunkSize + 1)).isOk(),
          "oversized chunk rejected");
    check(!storage.load("../../etc/passwd").isOk(),
          "path-like hash rejected");

    // Missing file.
    const QString missingHash = hashOf(makeBytes(77));
    check(!storage.load(missingHash).isOk(),
          "missing chunk is an error");
    check(!storage.exists(missingHash),
          "exists is false for missing chunk");

    const QString path = QDir(dir.path()).filePath("chunks/" + hash);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly),
          "test can open stored chunk file");
    const QByteArray good = file.readAll();
    file.close();

    check(overwrite(path, good.left(5)),
          "write truncated-header fixture");
    check(!storage.load(hash).isOk(),
          "truncated header detected");

    check(overwrite(path, good.left(good.size() - 10)),
          "write truncated-payload fixture");
    check(!storage.load(hash).isOk(),
          "truncated payload detected");

    QByteArray badMagic = good;
    badMagic[0] = 'X';
    check(overwrite(path, badMagic), "write bad-magic fixture");
    check(!storage.load(hash).isOk(), "bad magic detected");

    QByteArray badVersion = good;
    badVersion[4] = 9;
    check(overwrite(path, badVersion), "write bad-version fixture");
    check(!storage.load(hash).isOk(), "unknown version detected");

    QByteArray badMethod = good;
    badMethod[5] = 7;
    check(overwrite(path, badMethod), "write bad-method fixture");
    check(!storage.load(hash).isOk(), "unknown method detected");

    QByteArray flipped = good;
    flipped[20] = static_cast<char>(flipped[20] ^ 0x01);
    check(overwrite(path, flipped), "write hash-mismatch fixture");
    check(!storage.load(hash).isOk(),
          "flipped byte detected by hash check");

    // A file one byte beyond the raw maximum must be rejected before
    // readAll/allocation.
    const QByteArray oversized(
        static_cast<int>(kChunkSize + 14 + 1), 'x');
    check(overwrite(path, oversized), "write oversized fixture");
    check(!storage.load(hash).isOk(),
          "oversized stored chunk rejected");

    // A corrupt existing file is repaired by store().
    check(storage.store(hash, raw).isOk(),
          "store repairs corrupt file");
    const auto repaired = storage.load(hash);
    check(repaired.isOk() && repaired.value() == raw,
          "repaired chunk loads correctly");

    // Huffman chunks (BV-09): compressible data is stored with Huffman,
    // incompressible data stays raw, and damaged Huffman files are rejected.
    {
        Bytes compressible(kChunkSize);
        const char pattern[] = "aaaabbc ";
        for (std::size_t i = 0; i < compressible.size(); ++i)
            compressible[i] = static_cast<std::uint8_t>(pattern[i % 8]);
        const QString compHash = hashOf(compressible);

        const auto compStored = storage.store(compHash, compressible);
        check(compStored.isOk(), "store compressible chunk");
        check(compStored.isOk() && compStored.value().storedSize < kChunkSize / 2,
              "compressible chunk is stored in much less space");

        const QString compPath = QDir(dir.path()).filePath("chunks/" + compHash);
        QFile compFile(compPath);
        check(compFile.open(QIODevice::ReadOnly), "open Huffman chunk file");
        const QByteArray compGood = compFile.readAll();
        compFile.close();
        check(compGood.size() > 14 && compGood[5] == 1,
              "compressible chunk uses the Huffman method");
        check(compStored.isOk()
                  && static_cast<std::uint64_t>(compGood.size()) == compStored.value().storedSize,
              "storedSize matches the file size");

        const auto compLoaded = storage.load(compHash);
        check(compLoaded.isOk() && compLoaded.value() == compressible,
              "Huffman chunk loads back to the same bytes");
        const auto compAgain = storage.store(compHash, compressible);
        check(compAgain.isOk() && compStored.isOk()
                  && compAgain.value().storedSize == compStored.value().storedSize,
              "duplicate Huffman chunk is not rewritten");

        // A full chunk of one repeated byte.
        const Bytes zeros(kChunkSize, 0);
        const QString zerosHash = hashOf(zeros);
        check(storage.store(zerosHash, zeros).isOk(), "store all-zero chunk");
        const auto zerosLoaded = storage.load(zerosHash);
        check(zerosLoaded.isOk() && zerosLoaded.value() == zeros,
              "all-zero chunk round trip");

        // Random-looking data is not smaller with Huffman, so it stays raw.
        const Bytes noise = makeBytes(kChunkSize);
        const QString noiseHash = hashOf(noise);
        check(storage.store(noiseHash, noise).isOk(), "store incompressible chunk");
        QFile noiseFile(QDir(dir.path()).filePath("chunks/" + noiseHash));
        check(noiseFile.open(QIODevice::ReadOnly), "open raw chunk file");
        const QByteArray noiseData = noiseFile.readAll();
        noiseFile.close();
        check(noiseData.size() == static_cast<int>(kChunkSize) + 14 && noiseData[5] == 0,
              "incompressible chunk falls back to raw");

        // Damaged Huffman files.
        QByteArray damaged = compGood;
        damaged[14 + 256 + 5] = static_cast<char>(damaged[14 + 256 + 5] ^ 0x10);
        check(overwrite(compPath, damaged), "write damaged Huffman bits");
        check(!storage.load(compHash).isOk(), "damaged Huffman bit stream detected");

        damaged = compGood;
        damaged[14 + 97] = static_cast<char>(damaged[14 + 97] + 1);
        check(overwrite(compPath, damaged), "write damaged Huffman table");
        check(!storage.load(compHash).isOk(), "damaged Huffman table detected");

        damaged = compGood;
        damaged[6] = static_cast<char>(damaged[6] + 1);  // original size + 1
        check(overwrite(compPath, damaged), "write wrong original size");
        check(!storage.load(compHash).isOk(), "wrong original size detected");

        damaged = compGood;
        damaged[5] = 0;  // claims raw, but payload is Huffman data
        check(overwrite(compPath, damaged), "write wrong method");
        check(!storage.load(compHash).isOk(), "wrong method detected");

        check(overwrite(compPath, compGood.left(compGood.size() - 3)),
              "write truncated Huffman file");
        check(!storage.load(compHash).isOk(), "truncated Huffman file detected");

        // store() repairs a damaged Huffman file.
        check(storage.store(compHash, compressible).isOk(), "store repairs Huffman file");
        const auto compRepaired = storage.load(compHash);
        check(compRepaired.isOk() && compRepaired.value() == compressible,
              "repaired Huffman chunk loads");
    }

    // A symlinked chunk file must not be followed. Some Windows
    // configurations cannot create symlinks without additional privileges.
    QTemporaryDir linkTargetDir;
    check(linkTargetDir.isValid(), "symlink target temp dir created");

    const QString targetPath =
        QDir(linkTargetDir.path()).filePath("target");
    check(overwrite(targetPath, good), "create symlink target");

    const QString linkedFileHash = hashOf(makeBytes(123));
    const QString linkPath =
        QDir(dir.path()).filePath("chunks/" + linkedFileHash);

    const bool fileLinkCreated = QFile::link(targetPath, linkPath);
    if (fileLinkCreated && QFileInfo(linkPath).isSymLink()) {
        check(!storage.load(linkedFileHash).isOk(),
              "symlinked chunk file rejected");
        check(!storage.store(linkedFileHash, makeBytes(123)).isOk(),
              "store refuses symlinked chunk file");
        check(!storage.exists(linkedFileHash),
              "exists rejects symlinked chunk file");
    } else {
        std::cout
            << "SKIP: filesystem does not support creating test file symlinks"
            << std::endl;
        if (QFile::exists(linkPath))
            QFile::remove(linkPath);
    }

    // A symlinked chunks directory must not redirect storage outside the vault.
    QTemporaryDir outsideDir;
    check(outsideDir.isValid(), "external temp dir created");

    const QString outsideChunks =
        QDir(outsideDir.path()).filePath("chunks");
    check(QDir().mkpath(outsideChunks),
          "create external chunks directory");

    // Put a real chunk in the external directory. This ensures the exists()
    // check proves that it rejects the symlink, rather than merely reporting
    // that the requested chunk is absent.
    ChunkStorage outsideStorage(outsideDir.path());
    check(outsideStorage.store(hash, raw).isOk(),
          "create real chunk in external chunks directory");

    const QString linkedVault =
        QDir(dir.path()).filePath("linked-vault");
    check(QDir().mkpath(linkedVault),
          "create linked vault parent");

    const QString linkedChunks =
        QDir(linkedVault).filePath("chunks");

    const bool directoryLinkCreated =
        QFile::link(outsideChunks, linkedChunks);

    if (directoryLinkCreated && QFileInfo(linkedChunks).isSymLink()) {
        ChunkStorage linkedStorage(linkedVault);

        check(!linkedStorage.load(hash).isOk(),
              "symlinked chunks directory rejected by load");
        check(!linkedStorage.store(hash, raw).isOk(),
              "symlinked chunks directory rejected by store");
        check(!linkedStorage.exists(hash),
              "exists rejects symlinked chunks directory");
    } else {
        std::cout
            << "SKIP: filesystem does not support creating test directory symlinks"
            << std::endl;
        if (QFileInfo(linkedChunks).isSymLink())
            QFile::remove(linkedChunks);
    }

    if (failures == 0)
        std::cout << "All ChunkStorage checks passed." << std::endl;

    return failures == 0 ? 0 : 1;
}
