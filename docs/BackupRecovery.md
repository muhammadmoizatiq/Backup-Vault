# Backup Operation, Progress, and Orphan-Chunk Recovery

## Source selection contract

`BackupRequest::sourcePath` remains supported for the original single-folder
workflow. Its contents are stored at the snapshot root (`.`), preserving the
existing BV-04 layout.

For GUI multi-selection, use `BackupRequest::sourcePaths`. Each selected file
or directory is stored below the snapshot root using its selected base name.
A selected directory is represented as a directory entry, including empty
directories. Its descendants retain their relative paths. The GUI can assign
`FileSelectionWidget::selectedPaths()` directly to `request.sourcePaths`.

The engine rejects empty selections, symbolic-link roots, overlapping
selections, duplicate/case-colliding paths, and file/directory path conflicts.
It rejects a selection that is the vault or lies inside the vault. When the
vault is nested under a selected directory, the vault subtree is excluded.

Snapshot names are trimmed and must not be empty. The existing database schema
does not impose a unique constraint on snapshot names, so repeated names remain
permitted for compatibility. Snapshot IDs—not names—are the unique identifiers;
the GUI should display creation time or another discriminator when names repeat.

## Progress contract

`BackupProgressCallback` is optional. It is called synchronously on the
backup worker thread and reports the current path, completed/total file counts,
and processed/total bytes. The callback is observational: exceptions thrown
by a callback are contained and do not interrupt backup. A GUI must forward
updates to GUI widgets through a queued signal/connection; it must not update
widgets directly from this callback.

`bytesProcessed` counts bytes read from the source stream. `filesCompleted`
increments only after the file metadata and all ordered chunk links have been
recorded. The total byte/file counts are calculated from the scan before the
transaction begins.

## Worker-thread ownership

Construct `DatabaseManager`, open its connection, then construct
`ChunkStorage`, `ChunkIndex`, and `BackupEngine` inside the backup worker
thread. Use and destroy these objects in that same thread. Do not pass a
GUI-thread `DatabaseManager` or `QSqlDatabase` connection to a worker.
`DatabaseManager` records its construction thread and rejects opening or
reporting itself open from another thread. `BackupEngine` also records its
construction thread and rejects a call made from another thread. The vault
lock continues to serialize backup and restore operations.

## Orphaned physical chunk files

Chunk files are immutable and content-addressed by SHA-256. They are written
before the enclosing SQLite transaction commits because the database row
must never point to a chunk file that has not been durably stored.

If a later database operation fails, SQLite rollback removes the uncommitted
chunk metadata and file links, but already-written chunk files may remain.
These files are harmless orphans: they are not reachable from a committed
snapshot, and their content hash is still verified whenever they are loaded.

**Do not delete chunk files in the backup rollback path.** A chunk may be
shared by multiple files or snapshots, and deleting based on the failed
operation's local state could corrupt an existing backup. Existing database
rows and reference counts remain authoritative. Physical orphan reclamation
belongs to the dedicated garbage-collection work (BV-13), which must run
under the same exclusive vault lock and distinguish files absent from the
database from chunks with a database row and a nonzero reference count.
A crash may leave the same kind of orphan, so cleanup must be repeatable and
must never be required for restore correctness.
