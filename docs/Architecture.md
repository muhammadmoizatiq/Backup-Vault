# BackupVault — System Architecture and Shared Interfaces

**Task:** BV-01 — Design overall system architecture and agree shared interfaces  
**Status:** Draft for team review  
**Team:** Bro Code — Qasim, Abdul Rehman, Moiz  
**Project:** C++17 / Qt 6 desktop backup application

> Approval note: The decisions below record the team's proposed direction from the BV-01 discussion. A recommendation marked “Approve” is not proof that a teammate has approved it. Record actual approval in the decision table before treating a contract as final.

## 1. Purpose

BackupVault backs up selected folders by scanning their contents, splitting file data into fixed-size chunks, detecting duplicate chunks, compressing and storing new chunks, saving metadata in SQLite, and restoring files from a selected snapshot.

The architecture is divided into three cooperating areas:

- **Core / Backup Engine — Qasim:** scanning, chunking, hashing, deduplication lookup, backup and restore orchestration.
- **Storage, Compression and Database — Abdul Rehman:** chunk file storage, Huffman compression, SQLite metadata, integrity and reference-count operations.
- **GUI and Integration — Moiz:** Qt user interface, profiles, snapshots, progress display and connection of UI actions to the engines.

## 2. High-level architecture

```text
                       Qt GUI (Moiz)
                 Profiles / Snapshots / Status
                            |
                            v
                   Backup / Restore Engine
                         (Qasim)
                            |
             +--------------+--------------+
             |                             |
             v                             v
        File Scanner                    Chunker
      Stack DFS / Queue BFS          64 KiB chunks
             |                             |
             +--------------+--------------+
                            |
                            v
                    SHA-256 / ChunkIndex
                  in-memory lookup cache
                            |
                            v
                  ChunkStorage (Abdul)
                store(hash, rawBytes)
                            |
                    Huffman internally
                            |
                            v
                      chunks/ files
                            |
                            v
                 DatabaseManager (Abdul)
                     SQLite / QSQLITE
                            |
                            v
                 storage/backups/vault.db
```

`ChunkStorage` uses the Huffman module internally. The backup engine does not need to know whether a chunk was compressed or stored using the raw fallback format.

## 3. Main backup workflow

1. The user selects a source folder and starts a backup in the GUI.
2. The GUI asks the backup engine to start the job.
3. The engine acquires the vault-wide lock.
4. The scanner walks the folder structure. Directories are represented as entries; symbolic links are skipped with a warning.
5. The chunker reads file contents in chunks of `kChunkSize` (65,536 bytes).
6. The engine computes each file's SHA-256 content hash while reading it and computes a SHA-256 hash for each chunk.
7. The engine checks `ChunkIndex` / the database for an existing chunk.
8. If the chunk is new, the engine calls `ChunkStorage::store(hash, rawBytes)`. `ChunkStorage` chooses Huffman compression or raw storage internally and writes the versioned chunk file.
9. The database records chunk metadata and the file-to-chunk relationship. Snapshot/reference-count changes are performed transactionally through the agreed protocol.
10. The engine releases the lock and reports the result to the GUI.

## 4. Main restore workflow

1. The user chooses a snapshot and destination.
2. The restore engine loads the snapshot's file entries and ordered chunk references from the database.
3. For each required chunk, storage reads the chunk file and decodes its on-disk format.
4. If the chunk is Huffman-compressed, `ChunkStorage` invokes Huffman decompression internally. Raw chunks are returned without decompression.
5. The restore engine writes the reconstructed file in the original chunk order.
6. The restored file's SHA-256 is checked against `files.content_hash`.
7. The engine reports success or a useful error to the GUI.

## 5. Shared conventions

| Item | Agreed direction |
|---|---|
| Namespace | `bv` |
| Language | C++17 |
| Qt | Qt 6 |
| Byte buffer | `using Bytes = std::vector<std::uint8_t>;` |
| Filesystem paths | `QString` |
| Hash algorithm | SHA-256 using `QCryptographicHash` |
| Hash in memory | 32 raw bytes |
| Hash in database and filenames | 64-character lowercase hexadecimal |
| Chunk size | `constexpr std::size_t kChunkSize = 65536;` |
| Error handling | `Result<T>` / `Status`; no exceptions across module boundaries |
| SQLite access | Qt SQL (`QSQLITE`) |
| Runtime locking | `QLockFile`, one exclusive vault-wide operation |
| Threading | Engines run in worker threads; one database connection per thread; queued signals to GUI |

### 5.1 Result and Status

Use one common result convention for public module boundaries. The exact implementation should be kept small and defined once in `include/common/`.

Conceptual usage:

```cpp
namespace bv {

template <typename T>
class Result; // Contains either a value or an error status.

class Status; // Represents success or an error.

}
```

Public methods should return `Result<T>` when they produce a value and `Status` when they only report success/failure. Do not throw exceptions across module boundaries. Internal implementation details may use normal local control flow, but errors must be converted to the shared result type before crossing a public interface.

The concrete constructors, error codes, and accessors for these types must be agreed by all three members before implementation.

### 5.2 Common folder

Create a shared `include/common/` area for types used by more than one module. It is co-owned by all three members. Any change to a common header requires review from Qasim, Abdul Rehman, and Moiz.

Suggested initial files:

```text
include/common/
├── Types.h
├── Result.h
└── Hash.h
```

Keep this folder small. Do not move module-specific data structures into it.

## 6. Module responsibilities and proposed public contracts

The following contracts describe ownership and expected responsibilities. Exact method signatures should be finalized by the relevant owners before implementation.

### 6.1 Qasim — Core

- `FileNode`: represents a file or directory in the scanned tree.
- `FileScanner`: scans directories using the required Stack-based DFS and Queue-based BFS.
- `Chunker`: reads files in 65,536-byte chunks.
- `ChunkIndex`: manually implemented hash table with linked-list chaining; in-memory cache of database chunk records.
- `BackupEngine`: coordinates scanning, hashing, deduplication, storage and snapshot creation.
- `RestoreEngine`: coordinates metadata lookup, chunk retrieval and file reconstruction.
- `SnapshotManager`: coordinates snapshot lifecycle and reference-count protocol with the database.

The `ChunkIndex` is a cache, not the source of truth. The database is authoritative for persisted metadata and reference counts.

### 6.2 Abdul Rehman — Storage, Compression and Database

**ChunkStorage**
- Accepts a chunk hash and raw bytes.
- Compresses internally using Huffman when beneficial; otherwise stores raw bytes.
- Writes and reads the versioned on-disk chunk format.
- Does not expose compression details to Qasim's engine.
- Must not delete a chunk unless the reference-count protocol allows it.

**Huffman**
- Implements Huffman encoding and decoding.
- Implements the required Min Heap manually; do not use `std::priority_queue`.
- Works with arbitrary binary data.
- Handles empty input and single-symbol input.
- Detects invalid or truncated encoded data.

**DatabaseManager**
- Uses Qt SQL with the `QSQLITE` driver.
- Creates and migrates the vault database schema.
- Stores profiles, snapshots, file entries, chunk metadata and ordered file-to-chunk relationships.
- Provides transactional reference-count operations.
- Uses prepared statements and reports errors through `Result<T>` / `Status`.

### 6.3 Moiz — GUI and integration

- Builds the Qt main window, dashboard, profile and snapshot screens.
- Starts backup and restore jobs without blocking the UI thread.
- Receives progress and completion updates through queued signals.
- Disables deletion controls while a backup/restore operation holds the vault lock.
- Displays warnings and errors in understandable language.

## 7. Chunk file format — version 1

Each stored chunk file must be self-describing so that a future reader can determine how to decode it.

The v1 format must contain:
- a fixed magic value identifying a BackupVault chunk;
- a format version;
- a storage method flag (`raw` or `Huffman`);
- the original uncompressed byte count;
- the encoded payload length;
- the payload bytes.

Rules:
- Huffman is used only when its complete stored representation is smaller than the raw representation.
- Otherwise the raw bytes are stored.
- The original size is required for validation and safe decoding.
- Unknown versions, invalid flags, impossible lengths, truncated headers and truncated payloads must return errors.
- The precise byte offsets, integer byte order, magic bytes and header size must be written as a small format specification before the first persistent implementation. Do not guess these values independently in different modules.

## 8. Deduplication and hashing

- Hash algorithm: SHA-256 via `QCryptographicHash`.
- In-memory hash representation: 32 raw bytes.
- Persistent hash representation: lowercase hexadecimal text (64 characters).
- Chunk filenames use the lowercase hexadecimal chunk hash.
- Deduplication is global within one vault.
- Whole-file SHA-256 is stored in `files.content_hash`.
- The backup engine computes the whole-file hash while it reads/chunks the file.
- `ChunkIndex` caches `ChunkRecord` values, while SQLite remains the source of truth.

## 9. Snapshot and reference-count protocol

A `SnapshotManager` is added to coordinate snapshot creation/deletion and chunk reference counts.

Required safety rules:
1. Snapshot metadata, file entries, file-to-chunk links and reference-count changes must be performed in a database transaction.
2. A chunk's reference count represents the number of retained file-chunk references using that chunk, according to the finalized schema.
3. Removing a snapshot removes its file entries and relationships and decreases the corresponding chunk reference counts in the same transaction.
4. A chunk file may be deleted only after the database confirms that its reference count is zero.
5. If a chunk file cannot be deleted, keep or recover metadata so that the vault does not silently lose track of stored data.
6. Deleting a profile keeps its snapshots. The profile reference in those snapshots becomes `NULL` (`ON DELETE SET NULL`).

The exact transaction sequence and recovery behavior for failures between filesystem operations and database commits must be documented in the storage/database implementation plan.

## 10. Concurrency and vault locking

- A single vault-wide `QLockFile` prevents conflicting backup, restore or deletion operations.
- Only one exclusive vault operation may run at a time.
- The GUI disables deletion while a backup is running.
- Long-running engines execute in worker threads.
- Each worker thread uses its own Qt SQL database connection. Do not share one `QSqlDatabase` connection across threads.
- Worker-to-GUI updates use queued signals; do not update widgets directly from worker threads.

## 11. Filesystem behavior and vault layout

- Directories are stored as entries, even when they contain no files.
- Symbolic links are skipped and produce a warning.
- Chunk size is 65,536 bytes.
- The vault root is configurable (planned follow-up: BV-18).
- The real application default should be outside the source-code repository.
- The database is stored at `storage/backups/vault.db` relative to the configured vault root.
- Chunk files are stored under `chunks/` relative to the configured vault root.

```text
<VaultRoot>/
├── chunks/
└── storage/
    └── backups/
        └── vault.db
```

## 12. Profile and progress data

The proposed profile fields are:
- profile name;
- source folder path;
- exclude rules.

The proposed job progress/result information includes:
- job identifier;
- operation type (backup or restore);
- current path;
- files completed and total files, when known;
- bytes processed and total bytes, when known;
- current status;
- warnings;
- final success/failure status and error details.

**Pending:** Moiz must confirm the profile fields and progress/result structure before these become shared interfaces.

## 13. Database and storage design

The detailed table definitions, constraints and indexes are maintained in [`DatabaseSchema.md`](DatabaseSchema.md). That document is part of BV-01 and must be reviewed together with this architecture.

## 14. Coding simplicity rule

All implementation must follow [`CodingStandards.md`](CodingStandards.md). The central rule is:

> Prefer the simplest implementation that correctly satisfies the requirements and that every team member can understand, explain, test and maintain.

Do not add abstractions, classes, dependencies or design patterns without a clear need. Simplicity must not weaken correctness, data safety, security or required DSA learning outcomes.

## 15. Project structure

```text
BackupVault/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── common/              # Shared types; all three review changes
│   ├── core/                # Qasim
│   ├── storage/             # Abdul Rehman
│   ├── compression/         # Abdul Rehman
│   ├── database/            # Abdul Rehman
│   └── gui/                 # Moiz
├── src/
│   ├── core/
│   ├── storage/
│   ├── compression/
│   ├── database/
│   └── gui/
├── tests/
└── docs/
    ├── Architecture.md
    ├── DatabaseSchema.md
    ├── CodingStandards.md
    └── WorkDivision.md
```

## 16. BV-01 decision and approval tracker

| ID | Decision | Current documentation status | Required reviewers / next action |
|---|---|---|---|
| D1 | `ChunkStorage` calls Huffman internally; engine calls `store(hash, raw)` | Included | Qasim and Abdul confirm |
| D2 | SHA-256; raw 32-byte in-memory hash; lowercase hex in DB and filenames | Included | Team confirm |
| D3 | Shared `include/common/`; all three review changes | Included | All three confirm |
| D4 | `Result<T>` / `Status`; no exceptions across boundaries | Included; exact type design pending | All three define/confirm |
| D5 | `Bytes`, `QString` paths, `bv` namespace | Included | Team confirm |
| D6 | Versioned chunk format v1 with raw fallback and canonical Huffman | High-level rules included; exact header layout still required | Abdul specifies; Qasim reviews |
| D7 | `ChunkIndex` is DB-backed in-memory cache; DB is source of truth | Included | Qasim and Abdul confirm |
| D8 | Reference-count protocol and `SnapshotManager` | Included; failure-recovery sequence needs implementation detail | All three confirm |
| D9 | Vault-wide `QLockFile`; GUI disables deletion during backup | Included | All three confirm |
| D10 | Worker threads, one DB connection per thread, queued signals | Included | All three confirm |
| D11 | Global per-vault dedup; profile deletion keeps snapshots (`SET NULL`) | Included | Moiz and Abdul confirm |
| D12 | Directories are entries; symlinks skipped with warning; 65,536-byte chunks | Included | Qasim and Abdul confirm |
| D13 | Whole-file SHA-256 in `files.content_hash` | Included | Qasim and Abdul confirm |
| D14 | Configurable vault root; chunks and `storage/backups/vault.db`; default outside repo | Included | Abdul and Moiz confirm |
| D15 | Profile fields and progress/result structure | Provisional only | Moiz must confirm |
| D16 | Qt SQL `QSQLITE`; no vendored SQLite amalgamation | Included | Abdul confirms |
| D17 | Extend existing `docs/Architecture.md`, do not replace it | This file is a proposed extension draft; existing repository copy must be merged, not overwritten blindly | Team review |

### Completion checklist

- [ ] Merge this draft into the repository's existing `docs/Architecture.md`.
- [ ] Review the architecture and ownership boundaries with Qasim and Moiz.
- [ ] Confirm the common `Result<T>`, `Status`, `Bytes` and hash definitions.
- [ ] Finalize the exact v1 chunk header layout.
- [ ] Finalize database schema and reference-count transaction behavior.
- [ ] Obtain Moiz's confirmation for D15.
- [ ] Record actual approvals for D1–D17.
- [ ] Update the decision tracker when an item is approved or changed.

**BV-01 should be marked complete only after the shared contracts are reviewed and agreed by the relevant team members.**
