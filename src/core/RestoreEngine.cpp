
#include "core/RestoreEngine.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>

#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bv {
namespace {

// ChunkStorage currently writes raw chunks only. The on-disk representation
// is a 14-byte header followed by at most kChunkSize payload bytes.
constexpr std::uint64_t kChunkHeaderSize = 14;
constexpr std::uint64_t kMaxRawStoredChunkSize =
    static_cast<std::uint64_t>(kChunkSize) + kChunkHeaderSize;

bool isSameOrInside(QString path, QString base)
{
    path = QDir::cleanPath(QDir::fromNativeSeparators(path));
    base = QDir::cleanPath(QDir::fromNativeSeparators(base));

#ifdef Q_OS_WIN
    path = path.toCaseFolded();
    base = base.toCaseFolded();
#endif

    if (path == base)
        return true;

    if (!base.endsWith('/'))
        base += '/';

    return path.startsWith(base);
}

bool isValidWindowsComponent(const QString &component)
{
    if (component.isEmpty() ||
        component.endsWith(QLatin1Char('.')) ||
        component.endsWith(QLatin1Char(' '))) {
        return false;
    }

    for (QChar ch : component) {
        if (ch.unicode() < 32 ||
            QStringLiteral("<>:\"|?*").contains(ch)) {
            return false;
        }
    }

    const QString base =
        component.section(QLatin1Char('.'), 0, 0).toUpper();

    if (base == QStringLiteral("CON") ||
        base == QStringLiteral("PRN") ||
        base == QStringLiteral("AUX") ||
        base == QStringLiteral("NUL")) {
        return false;
    }

    for (int i = 1; i <= 9; ++i) {
        if (base == QStringLiteral("COM%1").arg(i) ||
            base == QStringLiteral("LPT%1").arg(i)) {
            return false;
        }
    }

    return true;
}

bool validateEntries(
    const std::vector<FileEntryRecord> &entries,
    QString &error)
{
    std::unordered_map<std::string, QString> types;
    std::unordered_set<std::string> seen;
    bool hasRoot = false;

    for (const FileEntryRecord &entry : entries) {
        if (entry.entryType != QStringLiteral("file") &&
            entry.entryType != QStringLiteral("directory")) {
            error = QStringLiteral(
                "Snapshot contains an unsupported entry type.");
            return false;
        }

        if (entry.relativePath == QStringLiteral(".")) {
            if (entry.entryType != QStringLiteral("directory") ||
                entry.fileSize != 0 ||
                !entry.contentHash.isEmpty() ||
                hasRoot) {
                error = QStringLiteral(
                    "Snapshot contains invalid root-directory metadata.");
                return false;
            }

            hasRoot = true;
            continue;
        }

        const QString path = entry.relativePath;

        if (path.isEmpty() ||
            path.startsWith('/') ||
            path.contains('\\') ||
            path.contains(QChar::Null)) {
            error = QStringLiteral(
                "Snapshot contains an unsafe relative path.");
            return false;
        }

        const QStringList parts =
            path.split('/', Qt::KeepEmptyParts);

        for (const QString &part : parts) {
            if (part.isEmpty() ||
                part == QStringLiteral(".") ||
                part == QStringLiteral("..") ||
                !isValidWindowsComponent(part)) {
                error = QStringLiteral(
                    "Snapshot contains an unsafe path component: %1")
                            .arg(part);
                return false;
            }
        }

        const std::string key =
            path.toCaseFolded().toUtf8().toStdString();

        if (!seen.insert(key).second) {
            error = QStringLiteral(
                "Snapshot contains paths that collide on common filesystems: %1")
                        .arg(path);
            return false;
        }

        if (entry.entryType == QStringLiteral("directory")) {
            if (entry.fileSize != 0 ||
                !entry.contentHash.isEmpty()) {
                error = QStringLiteral(
                    "Snapshot contains invalid directory metadata: %1")
                            .arg(path);
                return false;
            }
        } else if (entry.contentHash.size() != 64) {
            error = QStringLiteral(
                "Snapshot contains an invalid file hash: %1")
                        .arg(path);
            return false;
        }

        types.emplace(key, entry.entryType);
    }

    if (!hasRoot) {
        error = QStringLiteral(
            "Snapshot is missing its root directory entry.");
        return false;
    }

    // Every non-root entry must have an explicitly recorded directory parent.
    for (const FileEntryRecord &entry : entries) {
        if (entry.relativePath == QStringLiteral("."))
            continue;

        QString parent = QFileInfo(entry.relativePath).path();

        if (parent == QStringLiteral("."))
            continue;

        while (!parent.isEmpty() &&
               parent != QStringLiteral(".")) {
            const std::string key =
                parent.toCaseFolded().toUtf8().toStdString();

            auto it = types.find(key);

            if (it == types.end() ||
                it->second != QStringLiteral("directory")) {
                error = QStringLiteral(
                    "Snapshot is missing a parent directory entry for: %1")
                            .arg(entry.relativePath);
                return false;
            }

            parent = QFileInfo(parent).path();
        }
    }

    return true;
}

bool writeRestoredFile(
    const QString &path,
    const FileEntryRecord &entry,
    const std::vector<QString> &hashes,
    DatabaseManager &database,
    ChunkStorage &storage,
    const QString &vaultRoot,
    std::uint64_t &chunksRead,
    QString &error)
{
    QSaveFile output(path);

    if (!output.open(QIODevice::WriteOnly)) {
        error = QStringLiteral(
            "Cannot create restored file '%1': %2")
                    .arg(path, output.errorString());
        return false;
    }

    QCryptographicHash fileHasher(QCryptographicHash::Sha256);
    std::uint64_t bytesWritten = 0;

    for (const QString &hash : hashes) {
        auto chunkRecord = database.getChunk(hash);

        if (!chunkRecord.isOk()) {
            output.cancelWriting();
            error = QStringLiteral(
                "Cannot retrieve metadata for chunk %1: %2")
                        .arg(hash, chunkRecord.message());
            return false;
        }

        const ChunkRecord &record = chunkRecord.value();

        if (record.hash != hash ||
            record.refCount <= 0 ||
            record.originalSize == 0 ||
            record.originalSize > kChunkSize ||
            record.storedSize < kChunkHeaderSize ||
            record.storedSize > kMaxRawStoredChunkSize ||
            record.storagePath != QStringLiteral("chunks/") + hash) {
            output.cancelWriting();

            error = record.storedSize > kMaxRawStoredChunkSize
                ? QStringLiteral(
                    "Chunk stored size exceeds the maximum supported raw-chunk size: %1")
                      .arg(hash)
                : QStringLiteral(
                    "Chunk metadata is invalid for hash %1.")
                      .arg(hash);

            return false;
        }

        const QString chunkPath =
            QDir(vaultRoot).filePath(record.storagePath);

        const QFileInfo chunkInfo(chunkPath);

        if (chunkInfo.isSymLink() ||
            !chunkInfo.exists() ||
            !chunkInfo.isFile() ||
            chunkInfo.size() < 0 ||
            static_cast<std::uint64_t>(chunkInfo.size()) !=
                record.storedSize) {
            output.cancelWriting();

            error = QStringLiteral(
                "Chunk file is missing, unsafe, or has an unexpected stored size: %1")
                        .arg(hash);
            return false;
        }

        auto loaded = storage.load(hash);

        if (!loaded.isOk()) {
            output.cancelWriting();

            error = QStringLiteral(
                "Cannot load chunk %1: %2")
                        .arg(hash, loaded.message());
            return false;
        }

        const Bytes &bytes = loaded.value();

        if (bytes.empty() ||
            bytes.size() != record.originalSize ||
            bytes.size() > kChunkSize) {
            output.cancelWriting();

            error = QStringLiteral(
                "Chunk size does not match database metadata: %1")
                        .arg(hash);
            return false;
        }

        const std::uint64_t chunkSize =
            static_cast<std::uint64_t>(bytes.size());

        // Check the declared file-size boundary BEFORE writing anything
        // from this chunk. Guard subtraction against unsigned underflow.
        if (bytesWritten > entry.fileSize ||
            chunkSize > entry.fileSize - bytesWritten) {
            output.cancelWriting();

            error = QStringLiteral(
                "Chunk would exceed the expected file size for: %1")
                        .arg(entry.relativePath);
            return false;
        }

        QByteArray data(
            reinterpret_cast<const char *>(bytes.data()),
            static_cast<qsizetype>(bytes.size()));

        // Handle partial writes instead of assuming one write writes
        // the entire chunk.
        qsizetype offset = 0;

        while (offset < data.size()) {
            const qint64 written =
                output.write(data.constData() + offset,
                             data.size() - offset);

            if (written <= 0) {
                output.cancelWriting();

                error = QStringLiteral(
                    "Cannot write restored file '%1': %2")
                            .arg(path, output.errorString());
                return false;
            }

            offset += static_cast<qsizetype>(written);
        }

        fileHasher.addData(data);
        bytesWritten += chunkSize;

        if (chunksRead ==
            std::numeric_limits<std::uint64_t>::max()) {
            output.cancelWriting();

            error = QStringLiteral(
                "Restored chunk counter overflow.");
            return false;
        }

        ++chunksRead;
    }

    if (bytesWritten != entry.fileSize) {
        output.cancelWriting();

        error = QStringLiteral(
            "Reconstructed size does not match snapshot metadata: %1")
                    .arg(entry.relativePath);
        return false;
    }

    const QString actualHash =
        QString::fromLatin1(fileHasher.result().toHex());

    if (actualHash != entry.contentHash) {
        output.cancelWriting();

        error = QStringLiteral(
            "Reconstructed file hash does not match snapshot metadata: %1")
                    .arg(entry.relativePath);
        return false;
    }

    if (!output.commit()) {
        error = QStringLiteral(
            "Cannot finalize restored file '%1': %2")
                    .arg(path, output.errorString());
        return false;
    }

    return true;
}

} // namespace

RestoreEngine::RestoreEngine(
    const QString &vaultRoot,
    DatabaseManager &database,
    ChunkStorage &storage)
    : m_vaultRoot(QDir::cleanPath(vaultRoot)),
      m_database(database),
      m_storage(storage)
{
}

Result<RestoreSummary> RestoreEngine::restore(
    const RestoreRequest &request)
{
    if (!m_database.isOpen()) {
        return Result<RestoreSummary>::failure(
            "Database must be open before restoring a snapshot.");
    }

    if (request.snapshotId <= 0) {
        return Result<RestoreSummary>::failure(
            "Snapshot ID must be positive.");
    }

    if (request.destinationPath.trimmed().isEmpty()) {
        return Result<RestoreSummary>::failure(
            "Restore destination cannot be empty.");
    }

    QFileInfo vaultInput(m_vaultRoot);

    if (vaultInput.isSymLink() ||
        !vaultInput.exists() ||
        !vaultInput.isDir()) {
        return Result<RestoreSummary>::failure(
            "Vault root is missing, invalid, or a symbolic link.");
    }

    const QString vaultCanonical =
        vaultInput.canonicalFilePath();

    if (vaultCanonical.isEmpty()) {
        return Result<RestoreSummary>::failure(
            "Cannot resolve the vault root.");
    }

    const QFileInfo chunksDirectory(
        QDir(vaultCanonical).filePath("chunks"));

    if (chunksDirectory.isSymLink() ||
        !chunksDirectory.exists() ||
        !chunksDirectory.isDir()) {
        return Result<RestoreSummary>::failure(
            "Vault chunks directory is missing, invalid, or a symbolic link.");
    }

    const QFileInfo destinationInput(request.destinationPath);

    if (destinationInput.isSymLink() ||
        destinationInput.exists()) {
        return Result<RestoreSummary>::failure(
            "Restore destination already exists; overwriting is not permitted.");
    }

    const QString destinationAbsolute =
        QDir::cleanPath(destinationInput.absoluteFilePath());

    const QString destinationName =
        QFileInfo(destinationAbsolute).fileName();

    if (destinationName.isEmpty() ||
        destinationName == QStringLiteral(".") ||
        destinationName == QStringLiteral("..")) {
        return Result<RestoreSummary>::failure(
            "Restore destination must name a new directory.");
    }

    const QString parentInput =
        QFileInfo(destinationAbsolute).absolutePath();

    QFileInfo parentInfo(parentInput);

    if (!parentInfo.exists() ||
        !parentInfo.isDir() ||
        !parentInfo.isWritable()) {
        return Result<RestoreSummary>::failure(
            "Restore destination parent must be an existing writable directory.");
    }

    const QString parentCanonical =
        parentInfo.canonicalFilePath();

    if (parentCanonical.isEmpty()) {
        return Result<RestoreSummary>::failure(
            "Cannot resolve restore destination parent.");
    }

    const QString destinationCanonicalCandidate =
        QDir(parentCanonical).filePath(destinationName);

    if (isSameOrInside(destinationCanonicalCandidate, vaultCanonical) ||
        isSameOrInside(vaultCanonical, destinationCanonicalCandidate)) {
        return Result<RestoreSummary>::failure(
            "Restore destination cannot overlap the backup vault.");
    }

    const QString lockPath =
        QDir(vaultCanonical).filePath("backupvault.lock");

    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);

    if (!lock.tryLock(0)) {
        return Result<RestoreSummary>::failure(
            "The vault is locked by another operation.");
    }

    auto snapshot = m_database.getSnapshot(request.snapshotId);

    if (!snapshot.isOk()) {
        return Result<RestoreSummary>::failure(snapshot.message());
    }

    auto entriesResult =
        m_database.getSnapshotFiles(request.snapshotId);

    if (!entriesResult.isOk()) {
        return Result<RestoreSummary>::failure(
            entriesResult.message());
    }

    const std::vector<FileEntryRecord> &entries =
        entriesResult.value();

    QString validationError;

    if (!validateEntries(entries, validationError)) {
        return Result<RestoreSummary>::failure(validationError);
    }

    QTemporaryDir staging(
        QDir(parentCanonical).filePath(
            ".backupvault-restore-XXXXXX"));

    if (!staging.isValid()) {
        return Result<RestoreSummary>::failure(
            "Cannot create a temporary restore directory beside the destination.");
    }

    staging.setAutoRemove(false);

    const QString stagingPath = staging.path();

    auto failRestore = [&](const QString &message) {
        QDir cleanup(stagingPath);

        if (!cleanup.removeRecursively()) {
            return Result<RestoreSummary>::failure(
                message +
                QStringLiteral(
                    " Temporary restore data could not be fully removed: ") +
                stagingPath);
        }

        return Result<RestoreSummary>::failure(message);
    };

    RestoreSummary summary;
    summary.snapshotId = request.snapshotId;

    // Create directories first so file creation never depends on row ordering.
    for (const FileEntryRecord &entry : entries) {
        if (entry.entryType != QStringLiteral("directory"))
            continue;

        ++summary.directoriesRestored;

        if (entry.relativePath == QStringLiteral("."))
            continue;

        const QString directoryPath =
            QDir(stagingPath).filePath(entry.relativePath);

        if (!QDir().mkpath(directoryPath)) {
            return failRestore(
                QStringLiteral(
                    "Cannot create restored directory: %1")
                    .arg(entry.relativePath));
        }
    }

    for (const FileEntryRecord &entry : entries) {
        if (entry.entryType != QStringLiteral("file"))
            continue;

        auto hashes =
            m_database.getFileChunkHashes(entry.id);

        if (!hashes.isOk()) {
            return failRestore(
                QStringLiteral(
                    "Cannot retrieve chunk order for '%1': %2")
                    .arg(entry.relativePath, hashes.message()));
        }

        if ((entry.fileSize == 0 &&
             !hashes.value().empty()) ||
            (entry.fileSize > 0 &&
             hashes.value().empty())) {
            return failRestore(
                QStringLiteral(
                    "Chunk references do not match file size: %1")
                    .arg(entry.relativePath));
        }

        const QString outputPath =
            QDir(stagingPath).filePath(entry.relativePath);

        if (!writeRestoredFile(
                outputPath,
                entry,
                hashes.value(),
                m_database,
                m_storage,
                vaultCanonical,
                summary.chunksRead,
                validationError)) {
            return failRestore(validationError);
        }

        if (summary.bytesRestored >
            std::numeric_limits<std::uint64_t>::max() -
                entry.fileSize) {
            return failRestore(
                QStringLiteral(
                    "Total restored byte count overflow."));
        }

        summary.bytesRestored += entry.fileSize;
        ++summary.filesRestored;
    }

    // Preserve QTemporaryDir's restrictive staging-root permissions.
    // Do not broaden access before publishing the restored directory.

    QDir destinationParent(parentCanonical);

    if (!destinationParent.rename(
            QFileInfo(stagingPath).fileName(),
            destinationName)) {
        return failRestore(
            "Cannot atomically publish the restored directory at the requested destination.");
    }

    // The staging directory has been renamed; prevent its destructor from
    // attempting to remove the old temporary path (which no longer exists).
    staging.setAutoRemove(false);

    return Result<RestoreSummary>::success(std::move(summary));
}

} // namespace bv
