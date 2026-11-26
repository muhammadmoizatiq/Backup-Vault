
#include "core/Chunker.h"

#include <QFile>
#include <QByteArray>

#include <utility>

namespace bv {

Result<std::vector<Bytes>> Chunker::splitFile(
    const QString& filePath
) {
    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {
        return Result<std::vector<Bytes>>::failure(
            QString("Could not open file: %1").arg(file.errorString())
        );
    }

    std::vector<Bytes> chunks;

    while (!file.atEnd()) {

        QByteArray data = file.read(
            static_cast<qint64>(kChunkSize)
        );

        if (data.isEmpty()) {

            if (file.error() != QFileDevice::NoError) {
                return Result<std::vector<Bytes>>::failure(
                    QString("Error reading file: %1")
                        .arg(file.errorString())
                );
            }

            if (!file.atEnd()) {
                return Result<std::vector<Bytes>>::failure(
                    "File reading stopped unexpectedly."
                );
            }

            break;
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

        chunks.push_back(std::move(chunk));
    }

    return Result<std::vector<Bytes>>::success(std::move(chunks));
}

} // namespace bv
