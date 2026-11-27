#include "core/BackupEngine.h"

#include "core/Chunker.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <utility>
#include <tuple>
#include <vector>

namespace bv {
namespace {

struct SourceEntry {
    QString path;
    QString relativePath;
    bool isDirectory = false;
};

QString normalizedPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QString() : QDir::cleanPath(canonical);
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
    return QDir::cleanPath(QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(path)));
}

QString sha256Hex(const QByteArray &digest)
{
    return QString::fromLatin1(digest.toHex());
}

QString collisionKey(QString path)
{
    path = QDir::fromNativeSeparators(QDir::cleanPath(path));
    // Snapshots must remain portable to case-insensitive filesystems.
    return path.toCaseFolded();
}

bool isPortablePathComponent(const QString &component)
{
    if (component.isEmpty() ||
        component.endsWith(QLatin1Char('.')) ||
        component.endsWith(QLatin1Char(' ')))
        return false;

    for (QChar ch : component) {
        if (ch.unicode() < 32 ||
            QStringLiteral("<>:\"|?*").contains(ch))
            return false;
    }

    const QString base =
        component.section(QLatin1Char('.'), 0, 0).toUpper();
    if (base == QStringLiteral("CON") ||
        base == QStringLiteral("PRN") ||
        base == QStringLiteral("AUX") ||
        base == QStringLiteral("NUL"))
        return false;

    for (int i = 1; i <= 9; ++i) {
        if (base == QStringLiteral("COM%1").arg(i) ||
            base == QStringLiteral("LPT%1").arg(i))
            return false;
    }
    return true;
}

bool isPortableRelativePath(const QString &path)
{
    if (path == QStringLiteral("."))
        return true;
    if (path.isEmpty() || path.startsWith('/') ||
        path.contains('\\') || path.contains(QChar(0)) ||
        QDir::isAbsolutePath(path))
        return false;

    const QStringList components = path.split('/', Qt::KeepEmptyParts);
    for (const QString &component : components) {
        if (component == QStringLiteral(".") ||
            component == QStringLiteral("..") ||
            !isPortablePathComponent(component))
            return false;
    }
    return true;
}

void reportProgress(const BackupProgressCallback &callback,
                    const BackupProgress &progress) noexcept
{
    if (!callback)
        return;
    try {
        callback(progress);
    } catch (...) {
        // UI/progress observers must not interrupt a data operation.
    }
}

} // namespace

BackupEngine::BackupEngine(
    const QString &vaultRoot,
    DatabaseManager &database,
    ChunkStorage &storage,
    ChunkIndex &index
)
    : m_vaultRoot(QDir::cleanPath(vaultRoot)),
      m_ownerThread(QThread::currentThread()),
      m_database(database),
      m_storage(storage),
      m_index(index)
{
}

Result<BackupSummary> BackupEngine::createBackup(
    const BackupRequest &request,
    const BackupProgressCallback &progress
)
{
    if (QThread::currentThread() != m_ownerThread) {
        return Result<BackupSummary>::failure(
            "BackupEngine must be used on the thread where it was constructed. "
            "Create the database, storage, index, and engine inside the worker thread."
        );
    }

    if (!m_database.isOpen()) {
        return Result<BackupSummary>::failure(
            "Database must be open before creating a backup."
        );
    }

    const bool multipleSelection = !request.sourcePaths.empty();
    if (multipleSelection && !request.sourcePath.trimmed().isEmpty()) {
        return Result<BackupSummary>::failure(
            "Specify either sourcePath or sourcePaths, not both."
        );
    }

    QStringList selectedPaths;
    if (multipleSelection) {
        selectedPaths = request.sourcePaths;
    } else if (!request.sourcePath.trimmed().isEmpty()) {
        selectedPaths.push_back(request.sourcePath);
    } else {
        return Result<BackupSummary>::failure(
            "At least one backup source must be selected."
        );
    }

    if (request.snapshotName.trimmed().isEmpty()) {
        return Result<BackupSummary>::failure(
            "Snapshot name cannot be empty."
        );
    }

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
    if (vaultCanonical.isEmpty()) {
        return Result<BackupSummary>::failure(
            "Cannot resolve the vault directory."
        );
    }

    const QString lockPath = QDir(vaultCanonical).filePath("backupvault.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(0)) {
        return Result<BackupSummary>::failure(
            "The vault is locked by another operation."
        );
    }

    struct SelectedSource {
        QString canonical;
        QString displayPath;
        bool isDirectory = false;
        QString rootName;
    };

    std::vector<SelectedSource> sources;
    sources.reserve(selectedPaths.size());

    for (const QString &selected : selectedPaths) {
        if (selected.trimmed().isEmpty()) {
            return Result<BackupSummary>::failure(
                "A selected source path is empty."
            );
        }

        QFileInfo inputInfo(selected);
        if (inputInfo.isSymLink()) {
            return Result<BackupSummary>::failure(
                "A selected source cannot be a symbolic link: " + selected
            );
        }
        if (!inputInfo.exists() || (!inputInfo.isDir() && !inputInfo.isFile()) ||
            !inputInfo.isReadable()) {
            return Result<BackupSummary>::failure(
                "A selected source must be an existing readable file or directory: " +
                selected
            );
        }

        const QString canonical = normalizedPath(selected);
        if (canonical.isEmpty()) {
            return Result<BackupSummary>::failure(
                "Cannot resolve selected source: " + selected
            );
        }
        if (isSameOrInside(canonical, vaultCanonical)) {
            return Result<BackupSummary>::failure(
                "A selected source cannot be the vault or a path inside it: " +
                selected
            );
        }

        const bool isDirectory = inputInfo.isDir();
        QString rootName = inputInfo.fileName();
        if (rootName.isEmpty())
            rootName = QFileInfo(canonical).fileName();
        if (rootName.isEmpty()) {
            return Result<BackupSummary>::failure(
                "Cannot determine a safe name for selected source: " + selected
            );
        }

        sources.push_back({canonical, selected, isDirectory, rootName});
    }

    // Overlapping selections duplicate content and make the selected-root
    // mapping ambiguous. Reject them rather than silently backing up twice.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = i + 1; j < sources.size(); ++j) {
            if (isSameOrInside(sources[i].canonical, sources[j].canonical) ||
                isSameOrInside(sources[j].canonical, sources[i].canonical)) {
                return Result<BackupSummary>::failure(
                    "Selected sources overlap or contain one another: " +
                    sources[i].displayPath + " and " + sources[j].displayPath
                );
            }
        }
    }

    // A single legacy folder retains its original layout: its contents are
    // stored below ".". Multiple selections are each stored by base name.
    const bool legacySingleFolder =
        !multipleSelection && sources.size() == 1 && sources.front().isDirectory;

    FileScanner scanner;
    BackupSummary summary;
    std::vector<SourceEntry> entries;
    entries.push_back({QString(), QStringLiteral("."), true});

    if (legacySingleFolder) {
        const SelectedSource &source = sources.front();
        const bool vaultInsideSource =
            isSameOrInside(vaultCanonical, source.canonical);

        auto scanResult = scanner.scan(source.canonical);
        if (!scanResult.isOk()) {
            return Result<BackupSummary>::failure(
                "Cannot scan source folder: " + scanResult.message()
            );
        }
        summary.warnings = scanner.warnings();

        const QString unreadablePrefix =
            QStringLiteral("Cannot read folder, left empty: ");
        for (const QString &warning : scanner.warnings()) {
            if (!warning.startsWith(unreadablePrefix))
                continue;
            const QString unreadablePath =
                warning.mid(unreadablePrefix.size());
            const QString unreadableCanonical = normalizedPath(unreadablePath);
            if (unreadableCanonical.isEmpty() ||
                !vaultInsideSource ||
                !isSameOrInside(unreadableCanonical, vaultCanonical)) {
                return Result<BackupSummary>::failure(
                    "Backup cannot continue because a source directory "
                    "could not be read: " + unreadablePath
                );
            }
        }

        std::vector<const FileNode *> stack{&scanResult.value()};
        bool skippedRoot = false;
        while (!stack.empty()) {
            const FileNode *node = stack.back();
            stack.pop_back();

            const QString lexical =
                QDir::cleanPath(QFileInfo(node->path()).absoluteFilePath());
            if (vaultInsideSource && isSameOrInside(lexical, vaultCanonical))
                continue;

            QFileInfo lexicalInfo(node->path());
            if (lexicalInfo.isSymLink())
                return Result<BackupSummary>::failure(
                    "A source entry became a symbolic link during backup: " +
                    node->path());

            const QString canonical = normalizedPath(node->path());
            if (canonical.isEmpty() ||
                !isSameOrInside(canonical, source.canonical)) {
                return Result<BackupSummary>::failure(
                    "A source entry disappeared or resolved outside the source: " +
                    node->path());
            }

            if (!skippedRoot) {
                skippedRoot = true;
            } else {
                entries.push_back({
                    canonical,
                    relativePathFor(source.canonical, canonical),
                    node->isDirectory()
                });
            }

            const auto &children = node->children();
            for (std::size_t i = children.size(); i > 0; --i)
                stack.push_back(&children[i - 1]);
        }
    } else {
        for (const SelectedSource &source : sources) {
            if (source.isDirectory) {
                const bool vaultInsideSource =
                    isSameOrInside(vaultCanonical, source.canonical);
                auto scanResult = scanner.scan(source.canonical);
                if (!scanResult.isOk()) {
                    return Result<BackupSummary>::failure(
                        "Cannot scan selected folder: " + scanResult.message()
                    );
                }
                for (const QString &warning : scanner.warnings()) {
                    summary.warnings.push_back(warning);
                    const QString prefix =
                        QStringLiteral("Cannot read folder, left empty: ");
                    if (warning.startsWith(prefix)) {
                        const QString unreadable =
                            normalizedPath(warning.mid(prefix.size()));
                        if (unreadable.isEmpty() || !vaultInsideSource ||
                            !isSameOrInside(unreadable, vaultCanonical)) {
                            return Result<BackupSummary>::failure(
                                "Backup cannot continue because a source directory "
                                "could not be read: " + warning.mid(prefix.size())
                            );
                        }
                    }
                }

                std::vector<const FileNode *> stack{&scanResult.value()};
                while (!stack.empty()) {
                    const FileNode *node = stack.back();
                    stack.pop_back();

                    const QString lexical =
                        QDir::cleanPath(QFileInfo(node->path()).absoluteFilePath());
                    if (vaultInsideSource &&
                        isSameOrInside(lexical, vaultCanonical))
                        continue;

                    QFileInfo lexicalInfo(node->path());
                    if (lexicalInfo.isSymLink()) {
                        return Result<BackupSummary>::failure(
                            "A source entry became a symbolic link during backup: " +
                            node->path()
                        );
                    }

                    const QString canonical = normalizedPath(node->path());
                    if (canonical.isEmpty() ||
                        !isSameOrInside(canonical, source.canonical)) {
                        return Result<BackupSummary>::failure(
                            "A source entry disappeared or resolved outside its selected folder: " +
                            node->path()
                        );
                    }

                    const QString internal =
                        relativePathFor(source.canonical, canonical);
                    const QString relative =
                        internal == QStringLiteral(".")
                            ? source.rootName
                            : source.rootName + '/' + internal;

                    entries.push_back({canonical, relative, node->isDirectory()});

                    const auto &children = node->children();
                    for (std::size_t i = children.size(); i > 0; --i)
                        stack.push_back(&children[i - 1]);
                }
            } else {
                entries.push_back({
                    source.canonical,
                    source.rootName,
                    false
                });
            }
        }
    }

    // Reject duplicate paths and file/directory parent conflicts before
    // opening the database transaction.
    std::vector<std::tuple<QString, QString, bool>> pathKeys;
    pathKeys.reserve(entries.size());
    for (const SourceEntry &entry : entries) {
        const QString &path = entry.relativePath;
        if (path.isEmpty() || path == QStringLiteral("."))
            continue;
        if (!isPortableRelativePath(path)) {
            return Result<BackupSummary>::failure(
                "Selected source contains a path that cannot be restored portably: " +
                path
            );
        }
        pathKeys.emplace_back(collisionKey(path), path, entry.isDirectory);
    }

    // Sort once so every possible parent appears before its descendants.
    // This keeps validation near O(n log n), even for very large selections.
    std::sort(pathKeys.begin(), pathKeys.end(),
              [](const auto &left, const auto &right) {
                  return std::get<0>(left) < std::get<0>(right);
              });

    QHash<QString, bool> seenPaths;
    for (const auto &pathKey : pathKeys) {
        const QString &key = std::get<0>(pathKey);
        const QString &displayPath = std::get<1>(pathKey);
        if (seenPaths.contains(key)) {
            return Result<BackupSummary>::failure(
                "Selected sources produce a duplicate or case-colliding path: " +
                displayPath
            );
        }

        qsizetype separator = key.indexOf('/');
        while (separator >= 0) {
            const QString parent = key.left(separator);
            if (seenPaths.contains(parent) && !seenPaths.value(parent)) {
                return Result<BackupSummary>::failure(
                    "A selected file conflicts with a child path: " +
                    displayPath
                );
            }
            separator = key.indexOf('/', separator + 1);
        }

        seenPaths.insert(key, std::get<2>(pathKey));
    }

    std::uint64_t totalBytes = 0;
    std::uint64_t totalFiles = 0;
    for (const SourceEntry &entry : entries) {
        if (entry.relativePath == QStringLiteral(".") || entry.isDirectory)
            continue;
        QFileInfo info(entry.path);
        if (!info.exists() || !info.isFile() || info.size() < 0) {
            return Result<BackupSummary>::failure(
                "Selected file is unavailable: " + entry.path
            );
        }
        const auto size = static_cast<std::uint64_t>(info.size());
        if (totalBytes > std::numeric_limits<std::uint64_t>::max() - size)
            return Result<BackupSummary>::failure("Total source size overflow.");
        totalBytes += size;
        ++totalFiles;
    }

    Status transaction = m_database.beginTransaction();
    if (!transaction.isOk())
        return Result<BackupSummary>::failure(transaction.message());

    auto failBackup = [&](const QString &message) {
        const Status rollbackStatus = m_database.rollback();
        m_index.clear();
        if (!rollbackStatus.isOk()) {
            return Result<BackupSummary>::failure(
                message + " Database rollback also failed: " +
                rollbackStatus.message()
            );
        }
        return Result<BackupSummary>::failure(message);
    };

    auto snapshotResult = m_database.createSnapshot(request.snapshotName.trimmed());
    if (!snapshotResult.isOk())
        return failBackup(snapshotResult.message());

    summary.snapshotId = snapshotResult.value();
    BackupProgress currentProgress;
    currentProgress.totalFiles = totalFiles;
    currentProgress.totalBytes = totalBytes;

    for (const SourceEntry &entry : entries) {
        currentProgress.currentPath = entry.path.isEmpty()
            ? QStringLiteral(".") : entry.path;
        reportProgress(progress, currentProgress);

        if (entry.relativePath == QStringLiteral(".")) {
            auto rootEntry = m_database.addFileEntry(
                summary.snapshotId, QStringLiteral("."), "directory", 0
            );
            if (!rootEntry.isOk())
                return failBackup(rootEntry.message());
            ++summary.directoriesBackedUp;
            continue;
        }

        QFileInfo before(entry.path);
        if (before.isSymLink())
            return failBackup(
                "Source entry became a symbolic link during backup: " + entry.path
            );

        const QString resolved = normalizedPath(entry.path);
        if (resolved.isEmpty() || resolved != entry.path)
            return failBackup(
                "Source entry disappeared or changed location: " + entry.path
            );

        if (entry.isDirectory) {
            if (!before.exists() || !before.isDir() || !before.isReadable())
                return failBackup(
                    "Directory became unavailable during backup: " + entry.path
                );
            auto added = m_database.addFileEntry(
                summary.snapshotId, entry.relativePath, "directory", 0
            );
            if (!added.isOk())
                return failBackup(added.message());
            ++summary.directoriesBackedUp;
            continue;
        }

        if (!before.exists() || !before.isFile() || !before.isReadable() ||
            before.size() < 0) {
            return failBackup(
                "File is missing, unreadable, or not a regular file: " + entry.path
            );
        }

        const std::uint64_t expectedSize =
            static_cast<std::uint64_t>(before.size());
        const QDateTime originalModified = before.lastModified();
        if (expectedSize > static_cast<std::uint64_t>(
                std::numeric_limits<qlonglong>::max())) {
            return failBackup("File is too large for the database: " + entry.path);
        }

        QCryptographicHash fileHasher(QCryptographicHash::Sha256);
        std::vector<QString> chunkHashes;
        std::uint64_t bytesRead = 0;

        Status streamStatus = Chunker::streamFile(
            entry.path,
            [&](const Bytes &chunk) -> Status {
                if (chunk.empty() || chunk.size() > kChunkSize)
                    return Status::failure("Invalid chunk size encountered: " + entry.path);
                if (bytesRead > std::numeric_limits<std::uint64_t>::max() - chunk.size())
                    return Status::failure("File size overflow while reading: " + entry.path);

                bytesRead += static_cast<std::uint64_t>(chunk.size());
                const char *data = reinterpret_cast<const char *>(chunk.data());
                fileHasher.addData(QByteArrayView(data, static_cast<qsizetype>(chunk.size())));

                const QString hashHex =
                    ChunkIndex::toHex(ChunkIndex::hashBytes(chunk));
                auto stored = m_storage.store(hashHex, chunk);
                if (!stored.isOk())
                    return Status::failure(stored.message());

                Status recorded = m_database.addChunk(
                    hashHex, static_cast<std::uint64_t>(chunk.size()), stored.value()
                );
                if (!recorded.isOk())
                    return recorded;

                chunkHashes.push_back(hashHex);
                ++summary.chunksProcessed;

                if (currentProgress.bytesProcessed >
                    std::numeric_limits<std::uint64_t>::max() - chunk.size())
                    return Status::failure("Progress byte counter overflow.");
                currentProgress.bytesProcessed +=
                    static_cast<std::uint64_t>(chunk.size());
                currentProgress.currentPath = entry.path;
                reportProgress(progress, currentProgress);
                return Status::success();
            }
        );

        if (!streamStatus.isOk())
            return failBackup(
                "Could not read file '" + entry.path + "': " + streamStatus.message()
            );

        QFileInfo after(entry.path);
        if (after.isSymLink() || normalizedPath(entry.path) != entry.path ||
            !after.exists() || !after.isFile() ||
            after.size() != before.size() ||
            after.lastModified() != originalModified ||
            bytesRead != expectedSize) {
            return failBackup("File changed while being backed up: " + entry.path);
        }

        const QString contentHash = sha256Hex(fileHasher.result());
        auto fileEntry = m_database.addFileEntry(
            summary.snapshotId, entry.relativePath, "file", bytesRead, contentHash
        );
        if (!fileEntry.isOk())
            return failBackup(fileEntry.message());

        for (std::size_t i = 0; i < chunkHashes.size(); ++i) {
            if (i > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return failBackup("File contains too many chunks: " + entry.path);
            Status linked = m_database.addFileChunk(
                fileEntry.value(), static_cast<int>(i), chunkHashes[i]
            );
            if (!linked.isOk())
                return failBackup(linked.message());
        }

        if (summary.originalBytes >
            std::numeric_limits<std::uint64_t>::max() - bytesRead)
            return failBackup("Total backup size overflow.");

        ++summary.filesBackedUp;
        summary.originalBytes += bytesRead;
        ++currentProgress.filesCompleted;
        reportProgress(progress, currentProgress);
    }

    Status committed = m_database.commit();
    if (!committed.isOk()) {
        const Status rollbackStatus = m_database.rollback();
        m_index.clear();
        QString message =
            "Backup transaction could not be committed: " + committed.message();
        if (!rollbackStatus.isOk())
            message += " Database rollback also failed: " + rollbackStatus.message();
        return Result<BackupSummary>::failure(message);
    }

    m_index.clear();
    reportProgress(progress, currentProgress);
    return Result<BackupSummary>::success(std::move(summary));
}

} // namespace bv
