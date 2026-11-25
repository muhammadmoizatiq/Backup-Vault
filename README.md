# BackupVault

A deduplicating backup tool written in C++17 with a Qt 6 Widgets GUI and SQLite storage.
Built by **Team Bro Code** (Qasim, Abdul Rehman, Moiz).

BackupVault scans a folder, splits files into fixed-size 64 KB chunks, skips chunks it has
already stored (deduplication), compresses new chunks with Huffman coding, and records each
backup as a snapshot that can be restored later.

## Data structures (implemented inside the modules, no separate dsa folder)
| Data structure | Where |
|---|---|
| N-ary Tree | `core/FileNode` |
| Stack (DFS) and Queue (BFS) | `core/FileScanner` |
| Hash Table with Linked List chaining | `core/ChunkIndex` |
| Min Heap and Huffman Tree | `compression/Huffman` |

## Team
| Member | Modules |
|---|---|
| Qasim | `core/` - backup engine, scanner, chunker, ChunkIndex, restore |
| Abdul Rehman | `storage/`, `compression/`, `database/` |
| Moiz | `gui/` and integration |

## Build
Requirements: CMake 3.16+, a C++17 compiler, Qt 6 (Widgets and Sql).

```bash
cmake -S . -B build
cmake --build build
./build/BackupVault
```

## Rules
- Work mainly inside your own folders; agree shared headers and function signatures first.
- Never push directly to `main`: use a feature branch and a pull request.
- Test your module before integrating it.
- Use GitHub issues, one responsible member per issue.

See `docs/` for the architecture notes, work division and backlog.
