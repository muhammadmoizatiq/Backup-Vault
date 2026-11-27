#include "core/Chunker.h"

#include <QFile>

#include <utility>

namespace bv {

Status Chunker::streamFile(
    const QString &filePath,
    const ChunkCallback &callback
) {
    if (!callback)
        return Status::failure("Chunk callback is empty.");

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {
        return Status::failure(
            QString("Could not open file: %1").arg(file.errorString())
        );
    }

    while (true) {
        QByteArray data = file.read(static_cast<qint64>(kChunkSize));

        if (data.isEmpty()) {
            if (file.error() != QFileDevice::NoError) {
                return Status::failure(
                    QString("Error reading file: %1").arg(file.errorString())
                );
            }

            if (file.atEnd())
                break;

            return Status::failure(
                "File reading stopped unexpectedly."
            );
        }

        Bytes chunk;
        chunk.reserve(static_cast<std::size_t>(data.size()));

        for (char byte : data) {
            chunk.push_back(
                static_cast<std::uint8_t>(
                    static_cast<unsigned char>(byte)
                )
            );
        }

        Status callbackStatus = callback(chunk);

        if (!callbackStatus.isOk())
            return callbackStatus;
    }

    return Status::success();
}

Result<std::vector<Bytes>> Chunker::splitFile(
    const QString &filePath
) {
    std::vector<Bytes> chunks;

    Status status = streamFile(
        filePath,
        [&chunks](const Bytes &chunk) {
            chunks.push_back(chunk);
            return Status::success();
        }
    );

    if (!status.isOk())
        return Result<std::vector<Bytes>>::failure(status.message());

    return Result<std::vector<Bytes>>::success(std::move(chunks));
}

} // namespace bv