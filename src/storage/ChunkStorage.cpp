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
    for (int i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
    return value;
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
    if (!isValidHash(hashHex))
        return false;
    return QFile::exists(chunkFilePath(hashHex));
}

Result<StoredChunk> ChunkStorage::store(const QString &hashHex, const Bytes &raw) const
{
    if (!isValidHash(hashHex))
        return Result<StoredChunk>::failure("Invalid chunk hash (need 64 lowercase hex characters).");
    if (raw.size() > kChunkSize)
        return Result<StoredChunk>::failure("Chunk is larger than the maximum chunk size.");

    QByteArray rawData(reinterpret_cast<const char *>(raw.data()), static_cast<int>(raw.size()));
    if (sha256Hex(rawData) != hashHex)
        return Result<StoredChunk>::failure("Chunk bytes do not match the given hash.");

    StoredChunk info;
    info.storagePath = "chunks/" + hashHex;

    // Deduplication: a valid copy is already stored. A corrupt copy is
    // not reused; it is replaced below.
    if (load(hashHex).isOk()) {
        info.storedSize = static_cast<std::uint64_t>(QFileInfo(chunkFilePath(hashHex)).size());
        return Result<StoredChunk>::success(info);
    }

    // Huffman will be tried here once BV-09 exists. For now: raw only.
    QByteArray fileData;
    fileData.append(kMagic, 4);
    fileData.append(static_cast<char>(kFormatVersion));
    fileData.append(static_cast<char>(kMethodRaw));
    appendUint32(fileData, static_cast<std::uint32_t>(raw.size()));  // original size
    appendUint32(fileData, static_cast<std::uint32_t>(raw.size()));  // payload size
    fileData.append(rawData);

    if (!QDir().mkpath(chunksDir()))
        return Result<StoredChunk>::failure("Cannot create chunks folder: " + chunksDir());

    // QSaveFile writes to a temp file and renames on commit, so a crash
    // never leaves a half-written chunk under the real name.
    QSaveFile file(chunkFilePath(hashHex));
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

    QFile file(chunkFilePath(hashHex));
    if (!file.exists())
        return Result<Bytes>::failure("Chunk file is missing: " + hashHex);
    if (!file.open(QIODevice::ReadOnly))
        return Result<Bytes>::failure("Cannot open chunk file: " + file.errorString());
    QByteArray fileData = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return Result<Bytes>::failure("Cannot read chunk file: " + file.errorString());

    if (fileData.size() < kHeaderSize)
        return Result<Bytes>::failure("Chunk file is truncated (header incomplete).");
    if (fileData.left(4) != QByteArray(kMagic, 4))
        return Result<Bytes>::failure("Not a BackupVault chunk file (bad magic).");

    std::uint8_t version = static_cast<std::uint8_t>(fileData[4]);
    std::uint8_t method = static_cast<std::uint8_t>(fileData[5]);
    std::uint32_t originalSize = readUint32(fileData, 6);
    std::uint32_t payloadSize = readUint32(fileData, 10);

    if (version != kFormatVersion)
        return Result<Bytes>::failure("Unsupported chunk format version.");
    if (method != kMethodRaw && method != kMethodHuffman)
        return Result<Bytes>::failure("Unknown chunk storage method.");
    if (originalSize > kChunkSize)
        return Result<Bytes>::failure("Chunk header has an impossible original size.");
    if (static_cast<std::uint64_t>(fileData.size()) != static_cast<std::uint64_t>(kHeaderSize) + payloadSize)
        return Result<Bytes>::failure("Chunk file size does not match its header (truncated or extra data).");

    QByteArray payload = fileData.mid(kHeaderSize);
    QByteArray rawData;
    if (method == kMethodRaw) {
        if (payloadSize != originalSize)
            return Result<Bytes>::failure("Raw chunk payload size does not match original size.");
        rawData = payload;
    } else {
        // Huffman decoding arrives with BV-09.
        return Result<Bytes>::failure("Huffman chunks are not supported yet (BV-09).");
    }

    if (sha256Hex(rawData) != hashHex)
        return Result<Bytes>::failure("Chunk is corrupt (hash mismatch): " + hashHex);

    Bytes raw(rawData.begin(), rawData.end());
    return Result<Bytes>::success(raw);
}

}  // namespace bv
