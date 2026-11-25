# BackupVault — Database Schema (BV-01 Draft)

**Status:** Proposed schema for team review  
**Database:** SQLite through Qt SQL (`QSQLITE`)  
**Database file:** `<VaultRoot>/storage/backups/vault.db`

> This is a starting schema proposal, not a claim that the team has approved every column. Confirm it with Qasim and Moiz before implementation, especially the file/chunk relationship and snapshot semantics.

## 1. Design rules

- SQLite is the source of truth for persisted metadata and chunk reference counts.
- Use prepared statements for values.
- Use transactions for snapshot creation/deletion and reference-count updates.
- Enable foreign-key enforcement on every database connection.
- Store SHA-256 hashes as 64-character lowercase hexadecimal text.
- Store paths as text.
- Use `ON DELETE SET NULL` for a profile deleted while its snapshots are retained.

## 2. Proposed tables

### `profiles`

Stores saved backup configurations.

| Column | Type | Rules |
|---|---|---|
| `id` | INTEGER | Primary key |
| `name` | TEXT | NOT NULL |
| `source_path` | TEXT | NOT NULL |
| `exclude_rules` | TEXT | NOT NULL; agreed serialization format required |
| `created_at` | TEXT | NOT NULL |

### `snapshots`

One completed backup point.

| Column | Type | Rules |
|---|---|---|
| `id` | INTEGER | Primary key |
| `profile_id` | INTEGER | Nullable; foreign key to `profiles(id)` with `ON DELETE SET NULL` |
| `name` | TEXT | Optional display name |
| `created_at` | TEXT | NOT NULL |
| `status` | TEXT | NOT NULL; allowed values to be finalized |

Deleting a profile must not delete its snapshots.

### `files`

One file or directory entry captured in a snapshot.

| Column | Type | Rules |
|---|---|---|
| `id` | INTEGER | Primary key |
| `snapshot_id` | INTEGER | NOT NULL; foreign key to `snapshots(id)` |
| `relative_path` | TEXT | NOT NULL |
| `entry_type` | TEXT | NOT NULL; `file` or `directory` |
| `file_size` | INTEGER | NOT NULL |
| `content_hash` | TEXT | Nullable for directories; SHA-256 hex for files |

Recommended uniqueness: `(snapshot_id, relative_path)`.

### `chunks`

One unique chunk stored in the vault.

| Column | Type | Rules |
|---|---|---|
| `hash` | TEXT | Primary key; 64 lowercase hex characters |
| `original_size` | INTEGER | NOT NULL |
| `stored_size` | INTEGER | NOT NULL |
| `storage_path` | TEXT | NOT NULL |
| `ref_count` | INTEGER | NOT NULL; default 0; cannot be negative |
| `created_at` | TEXT | NOT NULL |

### `file_chunks`

Ordered relationship between a file and its chunks.

| Column | Type | Rules |
|---|---|---|
| `file_id` | INTEGER | NOT NULL; foreign key to `files(id)` |
| `chunk_index` | INTEGER | NOT NULL; zero-based order within the file |
| `chunk_hash` | TEXT | NOT NULL; foreign key to `chunks(hash)` |

Primary key: `(file_id, chunk_index)`.

This table is necessary because a file may consist of multiple chunks and the original chunk order must be preserved during restore.

## 3. Proposed SQL

```sql
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    source_path TEXT NOT NULL,
    exclude_rules TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER,
    name TEXT,
    created_at TEXT NOT NULL,
    status TEXT NOT NULL,
    FOREIGN KEY (profile_id)
        REFERENCES profiles(id)
        ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id INTEGER NOT NULL,
    relative_path TEXT NOT NULL,
    entry_type TEXT NOT NULL,
    file_size INTEGER NOT NULL DEFAULT 0,
    content_hash TEXT,
    UNIQUE (snapshot_id, relative_path),
    FOREIGN KEY (snapshot_id)
        REFERENCES snapshots(id)
        ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS chunks (
    hash TEXT PRIMARY KEY,
    original_size INTEGER NOT NULL,
    stored_size INTEGER NOT NULL,
    storage_path TEXT NOT NULL,
    ref_count INTEGER NOT NULL DEFAULT 0 CHECK (ref_count >= 0),
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS file_chunks (
    file_id INTEGER NOT NULL,
    chunk_index INTEGER NOT NULL,
    chunk_hash TEXT NOT NULL,
    PRIMARY KEY (file_id, chunk_index),
    FOREIGN KEY (file_id)
        REFERENCES files(id)
        ON DELETE CASCADE,
    FOREIGN KEY (chunk_hash)
        REFERENCES chunks(hash)
);
```

## 4. Reference-count rule

The team must define whether `ref_count` counts each file-to-chunk occurrence or each distinct file reference. The simplest consistent proposal is:

- Every row inserted into `file_chunks` increments the referenced chunk's `ref_count` by one.
- Every row removed from `file_chunks` decrements that chunk's `ref_count` by one.
- These relationship and counter updates happen in the same database transaction.
- A chunk is eligible for physical deletion only when its committed `ref_count` is zero.
- The chunk file is deleted only after the database has confirmed the zero count while the vault-wide lock is held.

This counts repeated appearances of the same chunk within a file as separate references. Confirm this rule with Qasim and Abdul before coding.

## 5. Indexes

```sql
CREATE INDEX IF NOT EXISTS idx_snapshots_profile_id
    ON snapshots(profile_id);

CREATE INDEX IF NOT EXISTS idx_files_snapshot_id
    ON files(snapshot_id);

CREATE INDEX IF NOT EXISTS idx_file_chunks_chunk_hash
    ON file_chunks(chunk_hash);
```

## 6. Items requiring team confirmation

- Whether snapshots in progress are inserted immediately or only after successful completion.
- Exact allowed values and transitions for `snapshots.status`.
- Exact serialization format for `profiles.exclude_rules`.
- Whether `file_size` is required for directory entries (proposed value: zero).
- Confirmation that `ref_count` counts every `file_chunks` row.
- Filesystem/database recovery behavior if a chunk file write/delete succeeds but the database transaction fails.
- Whether file metadata needs timestamps or permissions for the assignment's restore requirements.
