#include "storage/ChunkStorage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace bv {

namespace {

// Chunk file header, version 1 (14 bytes, integers little-endian).
// See docs/ChunkFormat.md.
const char kMagic[4] = {'B', 'V', 'C', 'K'};
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::uint8_t kMethodRaw = 0;
constexpr std::uint8_t kMethodHuffman = 1;
constexpr int kHeaderSize = 14;
constexpr qint64 kMaxRawStoredChunkSize =
    static_cast<qint64>(kChunkSize) + kHeaderSize;

bool isValidHash(const QString &hashHex)
{
    if (hashHex.size() != 64)
        return false;
    for (QChar c : hashHex) {
        const bool isDigit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool isLowerHex = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!isDigit && !isLowerHex)
            return false;
    }
    return true;
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

void appendUint32(QByteArray &out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFF));
}

std::uint32_t readUint32(const QByteArray &data, int offset)
{
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(data[offset + i]))
                 << (8 * i);
    }
    return value;
}

bool chunksDirectoryIsSafe(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink())
        return false;
    return !info.exists() || info.isDir();
}

}  // namespace

ChunkStorage::ChunkStorage(const QString &vaultRoot) : m_vaultRoot(vaultRoot) {}

QString ChunkStorage::chunksDir() const
{
    return QDir(m_vaultRoot).filePath("chunks");
}

QString ChunkStorage::chunkFilePath(const QString &hashHex) const
{
    return QDir(chunksDir()).filePath(hashHex);
}

bool ChunkStorage::exists(const QString &hashHex) const
{
    if (!isValidHash(hashHex) || !chunksDirectoryIsSafe(chunksDir()))
        return false;

    const QFileInfo info(chunkFilePath(hashHex));
    return !info.isSymLink() && info.exists() && info.isFile();
}

Result<StoredChunk> ChunkStorage::store(const QString &hashHex, const Bytes &raw) const
{
    if (!isValidHash(hashHex))
        return Result<StoredChunk>::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (raw.size() > kChunkSize)
        return Result<StoredChunk>::failure("Chunk is larger than the maximum chunk size.");

    const QString directory = chunksDir();
    if (!chunksDirectoryIsSafe(directory))
        return Result<StoredChunk>::failure("Chunks path must be a real directory, not a symbolic link or non-directory.");

    const QString path = chunkFilePath(hashHex);
    const QFileInfo existingInfo(path);
    if (existingInfo.isSymLink())
        return Result<StoredChunk>::failure("Chunk file must not be a symbolic link: " + hashHex);
    if (existingInfo.exists() && !existingInfo.isFile())
        return Result<StoredChunk>::failure("Chunk path is not a regular file: " + hashHex);

    QByteArray rawData(reinterpret_cast<const char *>(raw.data()), static_cast<int>(raw.size()));
    if (sha256Hex(rawData) != hashHex)
        return Result<StoredChunk>::failure("Chunk bytes do not match the given hash.");

    StoredChunk info;
    info.storagePath = "chunks/" + hashHex;

    // Deduplication: a valid copy is already stored. A corrupt copy is
    // not reused; it is replaced below.
    if (load(hashHex).isOk()) {
        info.storedSize = static_cast<std::uint64_t>(QFileInfo(path).size());
        return Result<StoredChunk>::success(info);
    }

    // Huffman will be tried here once BV-09 exists. For now: raw only.
    QByteArray fileData;
    fileData.append(kMagic, 4);
    fileData.append(static_cast<char>(kFormatVersion));
    fileData.append(static_cast<char>(kMethodRaw));
    appendUint32(fileData, static_cast<std::uint32_t>(raw.size()));
    appendUint32(fileData, static_cast<std::uint32_t>(raw.size()));
    fileData.append(rawData);

    if (!QDir().mkpath(directory))
        return Result<StoredChunk>::failure("Cannot create chunks folder: " + directory);

    // Recheck after directory creation before opening the final path.
    if (!chunksDirectoryIsSafe(directory))
        return Result<StoredChunk>::failure("Chunks path must not be a symbolic link.");
    const QFileInfo finalInfo(path);
    if (finalInfo.isSymLink() || (finalInfo.exists() && !finalInfo.isFile()))
        return Result<StoredChunk>::failure("Chunk path is not a safe regular file: " + hashHex);

    // QSaveFile writes to a temporary file and publishes it on commit.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return Result<StoredChunk>::failure("Cannot open chunk file for writing: " + file.errorString());
    if (file.write(fileData) != fileData.size())
        return Result<StoredChunk>::failure("Cannot write chunk file: " + file.errorString());
    if (!file.commit())
        return Result<StoredChunk>::failure("Cannot finish chunk file: " + file.errorString());

    info.storedSize = static_cast<std::uint64_t>(fileData.size());
    return Result<StoredChunk>::success(info);
}

Result<Bytes> ChunkStorage::load(const QString &hashHex) const
{
    if (!isValidHash(hashHex))
        return Result<Bytes>::failure("Invalid chunk hash (need 64 lowercase hex characters).");

    const QString directory = chunksDir();
    if (!chunksDirectoryIsSafe(directory))
        return Result<Bytes>::failure("Chunks path must not be a symbolic link or non-directory.");

    const QString path = chunkFilePath(hashHex);
    const QFileInfo chunkInfo(path);
    if (chunkInfo.isSymLink())
        return Result<Bytes>::failure("Chunk file must not be a symbolic link: " + hashHex);
    if (!chunkInfo.exists())
        return Result<Bytes>::failure("Chunk file is missing: " + hashHex);
    if (!chunkInfo.isFile())
        return Result<Bytes>::failure("Chunk path is not a regular file: " + hashHex);
    if (chunkInfo.size() < kHeaderSize)
        return Result<Bytes>::failure("Chunk file is truncated (header incomplete).");
    if (chunkInfo.size() > kMaxRawStoredChunkSize)
        return Result<Bytes>::failure("Chunk file exceeds the maximum raw stored size.");

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Result<Bytes>::failure("Cannot open chunk file: " + file.errorString());

    // Read at most one byte beyond the raw-format maximum. This bounds
    // allocation even if the file changes after the QFileInfo size check.
    QByteArray fileData = file.read(kMaxRawStoredChunkSize + 1);
    if (file.error() != QFileDevice::NoError)
        return Result<Bytes>::failure("Cannot read chunk file: " + file.errorString());
    if (fileData.size() > kMaxRawStoredChunkSize || !file.atEnd())
        return Result<Bytes>::failure("Chunk file exceeds the maximum raw stored size.");

    if (fileData.size() < kHeaderSize)
        return Result<Bytes>::failure("Chunk file is truncated (header incomplete).");
    if (fileData.left(4) != QByteArray(kMagic, 4))
        return Result<Bytes>::failure("Not a BackupVault chunk file (bad magic).");

    const std::uint8_t version = static_cast<std::uint8_t>(fileData[4]);
    const std::uint8_t method = static_cast<std::uint8_t>(fileData[5]);
    const std::uint32_t originalSize = readUint32(fileData, 6);
    const std::uint32_t payloadSize = readUint32(fileData, 10);

    if (version != kFormatVersion)
        return Result<Bytes>::failure("Unsupported chunk format version.");
    if (method != kMethodRaw && method != kMethodHuffman)
        return Result<Bytes>::failure("Unknown chunk storage method.");
    if (originalSize > kChunkSize)
        return Result<Bytes>::failure("Chunk header has an impossible original size.");
    if (static_cast<std::uint64_t>(fileData.size())
        != static_cast<std::uint64_t>(kHeaderSize) + payloadSize) {
        return Result<Bytes>::failure("Chunk file size does not match its header (truncated or extra data).");
    }

    const QByteArray payload = fileData.mid(kHeaderSize);
    QByteArray rawData;
    if (method == kMethodRaw) {
        if (payloadSize != originalSize)
            return Result<Bytes>::failure("Raw chunk payload size does not match original size.");
        rawData = payload;
    } else {
        // Huffman decoding arrives with BV-09. Its maximum stored size
        // must be revisited when that format is implemented.
        return Result<Bytes>::failure("Huffman chunks are not supported yet (BV-09).");
    }

    if (sha256Hex(rawData) != hashHex)
        return Result<Bytes>::failure("Chunk is corrupt (hash mismatch): " + hashHex);

    Bytes raw(rawData.begin(), rawData.end());
    return Result<Bytes>::success(raw);
}

}  // namespace bv
