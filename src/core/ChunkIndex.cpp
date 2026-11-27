
#include "core/ChunkIndex.h"

#include <QByteArray>
#include <QCryptographicHash>

namespace bv {

ChunkIndex::ChunkIndex(DatabaseManager &database,
                       std::size_t bucketCount)
    : m_database(database),
      m_buckets(bucketCount == 0 ? 1 : bucketCount, nullptr),
      m_size(0)
{
}

ChunkIndex::~ChunkIndex()
{
    clear();
}

ChunkHash ChunkIndex::hashBytes(const Bytes &raw)
{
    const char *data = raw.empty()
        ? ""
        : reinterpret_cast<const char *>(raw.data());

    QByteArray digest = QCryptographicHash::hash(
        QByteArray(data, static_cast<qsizetype>(raw.size())),
        QCryptographicHash::Sha256
    );

    ChunkHash result{};

    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>(
            static_cast<unsigned char>(digest.at(static_cast<qsizetype>(i)))
        );
    }

    return result;
}

QString ChunkIndex::toHex(const ChunkHash &hash)
{
    QByteArray raw(
        reinterpret_cast<const char *>(hash.data()),
        static_cast<qsizetype>(hash.size())
    );

    return QString::fromLatin1(raw.toHex());
}

bool ChunkIndex::fromHex(const QString &hex, ChunkHash &hash)
{
    if (hex.size() != 64) {
        return false;
    }

    auto hexValue = [](QChar c) -> int {
        ushort value = c.unicode();

        if (value >= '0' && value <= '9') {
            return value - '0';
        }

        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }

        return -1;
    };

    for (std::size_t i = 0; i < hash.size(); ++i) {
        int high = hexValue(hex.at(static_cast<qsizetype>(i * 2)));
        int low = hexValue(hex.at(static_cast<qsizetype>(i * 2 + 1)));

        if (high < 0 || low < 0) {
            return false;
        }

        hash[i] = static_cast<std::uint8_t>((high << 4) | low);
    }

    return true;
}

std::size_t ChunkIndex::bucketFor(const ChunkHash &hash) const
{
    // FNV-1a hash over the 32 raw SHA-256 bytes.
    // This selects a bucket; it is not the content hash algorithm.
    std::size_t value = 2166136261u;

    for (std::uint8_t byte : hash) {
        value ^= static_cast<std::size_t>(byte);
        value *= 16777619u;
    }

    return value % m_buckets.size();
}

ChunkIndex::Node *ChunkIndex::findNode(const ChunkHash &hash) const
{
    std::size_t bucket = bucketFor(hash);
    Node *current = m_buckets[bucket];

    while (current != nullptr) {
        if (current->hash == hash) {
            return current;
        }

        current = current->next;
    }

    return nullptr;
}

Result<ChunkRecord> ChunkIndex::find(const ChunkHash &hash)
{
    // First, search the in-memory hash table.
    Node *cached = findNode(hash);

    if (cached != nullptr) {
        return Result<ChunkRecord>::success(cached->record);
    }

    // Cache miss: SQLite is the source of truth.
    const QString hashHex = toHex(hash);

    Result<bool> existence = m_database.chunkExists(hashHex);

    if (!existence.isOk()) {
        return Result<ChunkRecord>::failure(existence.message());
    }

    if (!existence.value()) {
        return Result<ChunkRecord>::failure(
            "Chunk does not exist: " + hashHex
        );
    }

    Result<ChunkRecord> databaseRecord = m_database.getChunk(hashHex);

    if (!databaseRecord.isOk()) {
        return Result<ChunkRecord>::failure(databaseRecord.message());
    }

    // Populate the cache for future lookups.
    Status cacheStatus = insert(databaseRecord.value());

    if (!cacheStatus.isOk()) {
        return Result<ChunkRecord>::failure(cacheStatus.message());
    }

    return Result<ChunkRecord>::success(databaseRecord.value());
}

Result<bool> ChunkIndex::exists(const ChunkHash &hash)
{
    // A cached record is known to exist in the database because
    // records are loaded from SQLite or inserted after persistence.
    if (findNode(hash) != nullptr) {
        return Result<bool>::success(true);
    }

    Result<bool> databaseResult = m_database.chunkExists(toHex(hash));

    if (!databaseResult.isOk()) {
        return Result<bool>::failure(databaseResult.message());
    }

    if (!databaseResult.value()) {
        return Result<bool>::success(false);
    }

    // Load the record into the cache.
    Result<ChunkRecord> record = m_database.getChunk(toHex(hash));

    if (!record.isOk()) {
        return Result<bool>::failure(record.message());
    }

    Status cacheStatus = insert(record.value());

    if (!cacheStatus.isOk()) {
        return Result<bool>::failure(cacheStatus.message());
    }

    return Result<bool>::success(true);
}

Status ChunkIndex::insert(const ChunkRecord &record)
{
    ChunkHash hash{};

    if (!fromHex(record.hash, hash)) {
        return Status::failure(
            "Invalid chunk hash: expected 64 lowercase hexadecimal characters."
        );
    }

    std::size_t bucket = bucketFor(hash);
    Node *current = m_buckets[bucket];

    // Update an existing entry instead of creating a duplicate.
    while (current != nullptr) {
        if (current->hash == hash) {
            current->record = record;
            return Status::success();
        }

        current = current->next;
    }

    // Insert a new node at the beginning of the bucket's linked list.
    Node *newNode = new Node(hash, record, m_buckets[bucket]);
    m_buckets[bucket] = newNode;
    ++m_size;

    return Status::success();
}

bool ChunkIndex::remove(const ChunkHash &hash)
{
    std::size_t bucket = bucketFor(hash);

    Node *current = m_buckets[bucket];
    Node *previous = nullptr;

    while (current != nullptr) {
        if (current->hash == hash) {
            if (previous == nullptr) {
                m_buckets[bucket] = current->next;
            } else {
                previous->next = current->next;
            }

            delete current;
            --m_size;

            return true;
        }

        previous = current;
        current = current->next;
    }

    return false;
}

void ChunkIndex::clear()
{
    for (Node *&head : m_buckets) {
        Node *current = head;

        while (current != nullptr) {
            Node *next = current->next;
            delete current;
            current = next;
        }

        head = nullptr;
    }

    m_size = 0;
}

std::size_t ChunkIndex::size() const
{
    return m_size;
}

} // namespace bv
