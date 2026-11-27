
#include "core/BackupEngine.h"

#include "core/Chunker.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

#include <limits>
#include <utility>
#include <vector>

namespace bv {

namespace {

QString normalizedPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return QString();

    return QDir::cleanPath(canonical);
}

bool isSameOrInside(const QString &path, const QString &parent)
{
    QString p = QDir::fromNativeSeparators(QDir::cleanPath(path));
    QString base = QDir::fromNativeSeparators(QDir::cleanPath(parent));

#ifdef Q_OS_WIN
    p = p.toCaseFolded();
    base = base.toCaseFolded();
#endif

    if (p == base)
        return true;

    if (!base.endsWith('/'))
        base += '/';

    return p.startsWith(base);
}

QString relativePathFor(const QString &root, const QString &path)
{
    if (QDir::cleanPath(root) == QDir::cleanPath(path))
        return QStringLiteral(".");

    QString relative = QDir(root).relativeFilePath(path);
    relative = QDir::fromNativeSeparators(relative);
    return QDir::cleanPath(relative);
}

QString sha256Hex(const QByteArray &digest)
{
    return QString::fromLatin1(digest.toHex());
}

} // namespace

BackupEngine::BackupEngine(
    const QString &vaultRoot,
    DatabaseManager &database,
    ChunkStorage &storage,
    ChunkIndex &index
)
    : m_vaultRoot(QDir::cleanPath(vaultRoot)),
      m_database(database),
      m_storage(storage),
      m_index(index)
{
}

Result<BackupSummary> BackupEngine::createBackup(
    const BackupRequest &request
)
{
    if (!m_database.isOpen()) {
        return Result<BackupSummary>::failure(
            "Database must be open before creating a backup."
        );
    }

    if (request.sourcePath.trimmed().isEmpty()) {
        return Result<BackupSummary>::failure(
            "Source folder path cannot be empty."
        );
    }

    // Reject a vault path that is itself a symbolic link.
    QFileInfo vaultInputInfo(m_vaultRoot);
    if (vaultInputInfo.isSymLink()) {
        return Result<BackupSummary>::failure(
            "The vault directory cannot be a symbolic link."
        );
    }

    if (!QDir().mkpath(m_vaultRoot)) {
        return Result<BackupSummary>::failure(
            "Cannot create vault directory: " + m_vaultRoot
        );
    }

    const QString vaultCanonical = normalizedPath(m_vaultRoot);

    // Check the original source path before canonicalization. Otherwise,
    // canonicalFilePath() would hide a source path that is a symlink.
    QFileInfo sourceInputInfo(request.sourcePath);

    if (sourceInputInfo.isSymLink()) {
        return Result<BackupSummary>::failure(
            "The source folder cannot be a symbolic link."
        );
    }

    if (!sourceInputInfo.exists() ||
        !sourceInputInfo.isDir() ||
        !sourceInputInfo.isReadable()) {
        return Result<BackupSummary>::failure(
            "Source must be an existing readable directory."
        );
    }

    const QString sourceCanonical = normalizedPath(request.sourcePath);

    if (vaultCanonical.isEmpty() || sourceCanonical.isEmpty()) {
        return Result<BackupSummary>::failure(
            "Cannot resolve the source or vault folder."
        );
    }

    // Backing up the vault itself or a folder inside it is unsafe.
    if (isSameOrInside(sourceCanonical, vaultCanonical)) {
        return Result<BackupSummary>::failure(
            "The source folder cannot be the vault or a folder inside the vault."
        );
    }

    // If the vault is inside the source, its subtree is excluded.
    const bool vaultInsideSource =
        isSameOrInside(vaultCanonical, sourceCanonical);

    const QString lockPath =
        QDir(vaultCanonical).filePath("backupvault.lock");

    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);

    if (!lock.tryLock(0)) {
        return Result<BackupSummary>::failure(
            "The vault is locked by another operation."
        );
    }

    auto scanResult = m_scanner.scan(sourceCanonical);

    if (!scanResult.isOk()) {
        return Result<BackupSummary>::failure(
            "Cannot scan source folder: " + scanResult.message()
        );
    }

    BackupSummary summary;
    summary.warnings = m_scanner.warnings();

    // An unreadable directory outside the excluded vault means the scan
    // is incomplete. Never create a successful snapshot in that situation.
    const QString unreadablePrefix =
        QStringLiteral("Cannot read folder, left empty: ");

    for (const QString &warning : m_scanner.warnings()) {
        if (!warning.startsWith(unreadablePrefix))
            continue;

        const QString unreadablePath =
            warning.mid(unreadablePrefix.size());

        const QString unreadableCanonical =
            normalizedPath(unreadablePath);

        // If the path disappeared, fail closed. If it is inside the vault,
        // it is excluded from this backup and does not affect completeness.
        if (unreadableCanonical.isEmpty() ||
            !vaultInsideSource ||
            !isSameOrInside(unreadableCanonical, vaultCanonical)) {
            return Result<BackupSummary>::failure(
                "Backup cannot continue because a source directory "
                "could not be read: " + unreadablePath
            );
        }
    }

    // Build the ordered list of nodes using DFS.
    std::vector<const FileNode *> orderedNodes;
    std::vector<const FileNode *> stack;
    stack.push_back(&scanResult.value());

    while (!stack.empty()) {
        const FileNode *current = stack.back();
        stack.pop_back();

        const QString lexicalPath =
            QDir::cleanPath(QFileInfo(current->path()).absoluteFilePath());

        // Exclude the vault before resolving its children.
        if (vaultInsideSource &&
            isSameOrInside(lexicalPath, vaultCanonical)) {
            continue;
        }

        QFileInfo lexicalInfo(current->path());

        // The scanner normally skips symlinks. If an entry was replaced
        // with a symlink after scanning, fail instead of following it.
        if (lexicalInfo.isSymLink()) {
            return Result<BackupSummary>::failure(
                "A source entry became a symbolic link during backup: " +
                current->path()
            );
        }

        const QString currentPath = normalizedPath(current->path());

        if (currentPath.isEmpty()) {
            return Result<BackupSummary>::failure(
                "A source entry disappeared or cannot be resolved: " +
                current->path()
            );
        }

        if (!isSameOrInside(currentPath, sourceCanonical)) {
            return Result<BackupSummary>::failure(
                "A source entry resolves outside the source directory: " +
                current->path()
            );
        }

        orderedNodes.push_back(current);

        const auto &children = current->children();

        for (std::size_t i = children.size(); i > 0; --i)
            stack.push_back(&children[i - 1]);
    }

    Status transaction = m_database.beginTransaction();

    if (!transaction.isOk()) {
        return Result<BackupSummary>::failure(
            transaction.message()
        );
    }

    auto failBackup = [&](const QString &message) {
        const Status rollbackStatus = m_database.rollback();

        // SQLite is authoritative; discard cached state after rollback.
        m_index.clear();

        if (!rollbackStatus.isOk()) {
            return Result<BackupSummary>::failure(
                message + " Database rollback also failed: " +
                rollbackStatus.message()
            );
        }

        return Result<BackupSummary>::failure(message);
    };

    auto snapshotResult = m_database.createSnapshot(
        request.snapshotName
    );

    if (!snapshotResult.isOk())
        return failBackup(snapshotResult.message());

    summary.snapshotId = snapshotResult.value();

    for (const FileNode *node : orderedNodes) {
        QFileInfo lexicalInfo(node->path());

        if (lexicalInfo.isSymLink()) {
            return failBackup(
                "A source entry became a symbolic link during backup: " +
                node->path()
            );
        }

        const QString path = normalizedPath(node->path());

        if (path.isEmpty() ||
            !isSameOrInside(path, sourceCanonical)) {
            return failBackup(
                "Source entry disappeared or resolved outside the source: " +
                node->path()
            );
        }

        const QString relativePath =
            relativePathFor(sourceCanonical, path);

        if (relativePath.isEmpty() ||
            relativePath == ".." ||
            relativePath.startsWith("../") ||
            relativePath.startsWith("..\\")) {
            return failBackup(
                "Unsafe relative path encountered: " + path
            );
        }

        QFileInfo before(path);

        if (before.isSymLink()) {
            return failBackup(
                "Source entry became a symbolic link during backup: " + path
            );
        }

        if (node->isDirectory()) {
            if (!before.exists() ||
                !before.isDir() ||
                !before.isReadable()) {
                return failBackup(
                    "Directory became unavailable during backup: " + path
                );
            }

            auto entry = m_database.addFileEntry(
                summary.snapshotId,
                relativePath,
                "directory",
                0
            );

            if (!entry.isOk())
                return failBackup(entry.message());

            ++summary.directoriesBackedUp;
            continue;
        }

        if (!before.exists() ||
            !before.isFile() ||
            !before.isReadable()) {
            return failBackup(
                "File is missing, unreadable, or not a regular file: " + path
            );
        }

        if (before.size() < 0) {
            return failBackup(
                "File has an invalid size: " + path
            );
        }

        const std::uint64_t expectedSize =
            static_cast<std::uint64_t>(before.size());

        const QDateTime originalModified = before.lastModified();

        if (expectedSize > static_cast<std::uint64_t>(
                std::numeric_limits<qlonglong>::max())) {
            return failBackup(
                "File is too large for the database: " + path
            );
        }

        QCryptographicHash fileHasher(QCryptographicHash::Sha256);
        std::vector<QString> chunkHashes;
        std::uint64_t bytesRead = 0;

        Status streamStatus = Chunker::streamFile(
            path,
            [&](const Bytes &chunk) -> Status {
                if (chunk.empty() || chunk.size() > kChunkSize) {
                    return Status::failure(
                        "Invalid chunk size encountered: " + path
                    );
                }

                if (bytesRead >
                    std::numeric_limits<std::uint64_t>::max() - chunk.size()) {
                    return Status::failure(
                        "File size overflow while reading: " + path
                    );
                }

                bytesRead += static_cast<std::uint64_t>(chunk.size());

                const char *data =
                    reinterpret_cast<const char *>(chunk.data());

                fileHasher.addData(
                    QByteArrayView(
                        data,
                        static_cast<qsizetype>(chunk.size())
                    )
                );

                const ChunkHash chunkHash =
                    ChunkIndex::hashBytes(chunk);

                const QString hashHex =
                    ChunkIndex::toHex(chunkHash);

                // store() validates the hash and safely reuses valid chunks.
                auto stored = m_storage.store(hashHex, chunk);

                if (!stored.isOk())
                    return Status::failure(stored.message());

                Status recorded = m_database.addChunk(
                    hashHex,
                    static_cast<std::uint64_t>(chunk.size()),
                    stored.value()
                );

                if (!recorded.isOk())
                    return recorded;

                chunkHashes.push_back(hashHex);
                ++summary.chunksProcessed;

                return Status::success();
            }
        );

        if (!streamStatus.isOk()) {
            return failBackup(
                "Could not read file '" + path + "': " +
                streamStatus.message()
            );
        }

        // Re-check the original path as well as the resolved path.
        QFileInfo lexicalAfter(node->path());
        const QString resolvedAfter = normalizedPath(node->path());
        QFileInfo after(path);

        if (lexicalAfter.isSymLink() ||
            resolvedAfter != path ||
            !after.exists() ||
            !after.isFile() ||
            after.size() != before.size() ||
            after.lastModified() != originalModified ||
            bytesRead != expectedSize) {
            return failBackup(
                "File changed while being backed up: " + path
            );
        }

        const QString contentHash =
            sha256Hex(fileHasher.result());

        auto fileEntry = m_database.addFileEntry(
            summary.snapshotId,
            relativePath,
            "file",
            bytesRead,
            contentHash
        );

        if (!fileEntry.isOk())
            return failBackup(fileEntry.message());

        for (std::size_t i = 0; i < chunkHashes.size(); ++i) {
            if (i > static_cast<std::size_t>(
                    std::numeric_limits<int>::max())) {
                return failBackup(
                    "File contains too many chunks: " + path
                );
            }

            Status linked = m_database.addFileChunk(
                fileEntry.value(),
                static_cast<int>(i),
                chunkHashes[i]
            );

            if (!linked.isOk())
                return failBackup(linked.message());
        }

        if (summary.originalBytes >
            std::numeric_limits<std::uint64_t>::max() - bytesRead) {
            return failBackup(
                "Total backup size overflow."
            );
        }

        ++summary.filesBackedUp;
        summary.originalBytes += bytesRead;
    }

    Status committed = m_database.commit();

    if (!committed.isOk()) {
        const Status rollbackStatus = m_database.rollback();
        m_index.clear();

        QString message =
            "Backup transaction could not be committed: " +
            committed.message();

        if (!rollbackStatus.isOk()) {
            message += " Database rollback also failed: " +
                       rollbackStatus.message();
        }

        return Result<BackupSummary>::failure(message);
    }

    // Clear cache after commit; SQLite remains authoritative.
    m_index.clear();

    return Result<BackupSummary>::success(std::move(summary));
}

} // namespace bv
