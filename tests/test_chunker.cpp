
#include "core/Chunker.h"

#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <vector>

using namespace bv;

int testsPassed = 0;
int testsFailed = 0;

void check(bool condition, const char* testName)
{
    if (condition) {
        std::cout << "[PASS] " << testName << '\n';
        testsPassed++;
    } else {
        std::cout << "[FAIL] " << testName << '\n';
        testsFailed++;
    }
}

bool createTestFile(const QString& path, const Bytes& data)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (file.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<qint64>(data.size())
    ) != static_cast<qint64>(data.size())) {
        return false;
    }

    return file.flush();
}

Bytes generateData(std::size_t size)
{
    Bytes data;
    data.reserve(size);

    for (std::size_t i = 0; i < size; i++) {
        data.push_back(static_cast<std::uint8_t>(i % 256));
    }

    return data;
}

bool checkChunkSizes(
    const std::vector<Bytes>& chunks,
    std::size_t expectedCount,
    std::size_t originalSize
) {
    if (chunks.size() != expectedCount) {
        return false;
    }

    std::size_t totalSize = 0;

    for (std::size_t i = 0; i < chunks.size(); i++) {

        if (chunks[i].empty() || chunks[i].size() > kChunkSize) {
            return false;
        }

        if (i < chunks.size() - 1 &&
            chunks[i].size() != kChunkSize) {
            return false;
        }

        totalSize += chunks[i].size();
    }

    return totalSize == originalSize;
}

Bytes combineChunks(const std::vector<Bytes>& chunks)
{
    Bytes combined;

    for (const Bytes& chunk : chunks) {
        combined.insert(
            combined.end(),
            chunk.begin(),
            chunk.end()
        );
    }

    return combined;
}

int main()
{
    QTemporaryDir tempDir;

    check(tempDir.isValid(), "Temporary directory creation");

    if (!tempDir.isValid()) {
        return 1;
    }

    // Test 1: Empty file
    {
        QString path = tempDir.filePath("empty.bin");
        Bytes original;

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created && result.isOk() && result.value().empty(),
            "Empty file produces zero chunks"
        );
    }

    // Test 2: 1 KB file
    {
        QString path = tempDir.filePath("small.bin");
        Bytes original = generateData(1024);

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            result.value().size() == 1 &&
            result.value()[0] == original,
            "1 KB file produces one correct chunk"
        );
    }

    // Test 3: Exactly 64 KB
    {
        QString path = tempDir.filePath("exact.bin");
        Bytes original = generateData(kChunkSize);

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            checkChunkSizes(result.value(), 1, original.size()) &&
            combineChunks(result.value()) == original,
            "Exactly 64 KB produces one chunk"
        );
    }

    // Test 4: 100 KB
    {
        QString path = tempDir.filePath("100kb.bin");
        Bytes original = generateData(100 * 1024);

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            checkChunkSizes(result.value(), 2, original.size()) &&
            combineChunks(result.value()) == original,
            "100 KB produces two chunks"
        );
    }

    // Test 5: Exactly 128 KB
    {
        QString path = tempDir.filePath("128kb.bin");
        Bytes original = generateData(2 * kChunkSize);

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            checkChunkSizes(result.value(), 2, original.size()) &&
            combineChunks(result.value()) == original,
            "Exactly 128 KB produces two chunks"
        );
    }

    // Test 6: 150 KB
    {
        QString path = tempDir.filePath("150kb.bin");
        Bytes original = generateData(150 * 1024);

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            checkChunkSizes(result.value(), 3, original.size()) &&
            combineChunks(result.value()) == original,
            "150 KB produces three chunks"
        );
    }

    // Test 7: Binary data preservation
    {
        QString path = tempDir.filePath("binary.bin");

        Bytes original = {
            0x00, 0x01, 0x7F, 0x80,
            0xFE, 0xFF, 0x00, 0xAA
        };

        bool created = createTestFile(path, original);
        auto result = Chunker::splitFile(path);

        check(
            created &&
            result.isOk() &&
            result.value().size() == 1 &&
            result.value()[0] == original,
            "Binary data is preserved correctly"
        );
    }

    // Test 8: Non-existent file
    {
        QString path = tempDir.filePath("missing.bin");

        auto result = Chunker::splitFile(path);

        check(
            !result.isOk(),
            "Non-existent file returns failure"
        );
    }

    std::cout << "\n========== CHUNKER TEST SUMMARY ==========\n";
    std::cout << "Passed: " << testsPassed << '\n';
    std::cout << "Failed: " << testsFailed << '\n';
    std::cout << "==========================================\n";

    return testsFailed == 0 ? 0 : 1;
}
