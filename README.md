# BackupVault

A deduplicating backup tool written in **C++17** with a **Qt 6 Widgets GUI** and **SQLite storage**.

Built by **Team Bro Code** (Qasim, Abdul Rehman, Moiz).

BackupVault scans a folder, splits files into fixed-size 64 KB chunks, skips chunks it has already stored (deduplication), compresses new chunks using Huffman coding, and records each backup as a snapshot that can be restored later.

## Project Structure

```text
Backup-Vault/
├── docs/
├── include/
│   ├── common/
│   ├── compression/
│   ├── core/
│   ├── database/
│   ├── gui/
│   └── storage/
├── src/
│   ├── compression/
│   ├── core/
│   ├── database/
│   ├── gui/
│   ├── storage/
│   └── main.cpp
├── storage/
├── tests/
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## Data Structures

All data structures are implemented inside their respective modules. There is no separate `dsa/` folder.

| Data Structure | Location |
|---|---|
| N-ary Tree | `core/FileNode` |
| Stack (DFS) and Queue (BFS) | `core/FileScanner` |
| Hash Table with Linked List Chaining | `core/ChunkIndex` |
| Min Heap and Huffman Tree | `compression/Huffman` |

## Team

| Member | Responsibilities |
|---|---|
| Qasim | `core/` – Backup Engine, Scanner, Chunker, ChunkIndex, Restore |
| Abdul Rehman | `storage/`, `compression/`, `database/` |
| Moiz | `gui/` and integration |

## Requirements

- CMake 3.16 or later
- C++17 compatible compiler
- Qt 6 with Widgets and Sql modules
- MinGW or another supported C++ toolchain

## Build Instructions

### Windows (MinGW)

Open a terminal in the project root and run:

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Run the application:

```bash
.\build\BackupVault.exe
```

### Linux / macOS

```bash
cmake -S . -B build
cmake --build build
./build/BackupVault
```

## Development Rules

- Work mainly inside your assigned module folders.
- Agree on shared headers and function signatures before implementation.
- Never push directly to `main`; use a feature branch and pull request.
- Test your module before integrating it.
- Use GitHub Issues to track tasks, with one responsible member per issue.
- Follow the agreed project architecture and coding standards.

## Storage

The `storage/` directory is used for vault data generated at runtime. Actual backup data should not be committed to GitHub.

## Documentation

See the `docs/` directory for architecture notes, database schema, work division, coding standards, and project backlog.