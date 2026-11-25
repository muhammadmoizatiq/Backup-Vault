# Backlog

Keep this in sync with the GitHub Project board.


## PRIORITY 1 - CRITICAL (Must Have)

| ID | Backlog Item | Assigned To |
|---|---|---|
| BV-01 | Design overall system architecture and agree shared interfaces | All |
| BV-02 | Create project folder structure and CMake configuration | All |
| BV-03 | Implement file and folder selection | Moiz |
| BV-04 | Implement backup creation engine and snapshot creation | Qasim |
| BV-05 | Implement ChunkStorage to save and retrieve chunk files | Abdul Rehman |
| BV-06 | Implement SQLite database, chunk metadata and reference counts | Abdul Rehman |
| BV-07 | Implement restore engine and file reconstruction in chunk order | Qasim |
| BV-08 | Connect GUI with backup engine | Moiz |
| BV-29 | Implement 64 KB Chunker | Qasim |
| BV-30 | Implement N-ary Tree (FileNode) and Stack/Queue DFS/BFS in FileScanner | Qasim |
| BV-31 | Implement ChunkIndex - Hash Table with Linked List chaining for deduplication | Qasim |

## PRIORITY 2 - HIGH (Core Features)

| ID | Backlog Item | Assigned To |
|---|---|---|
| BV-09 | Implement Huffman compression and decompression | Abdul Rehman |
| BV-32 | Implement Min Heap and Huffman Tree inside the Huffman module | Abdul Rehman |
| BV-10 | Implement backup history (snapshot browser) | Moiz |
| BV-11 | Implement incremental backup using chunk deduplication | Qasim |
| BV-12 | Implement backup integrity verification using chunk hashes | Abdul Rehman |
| BV-13 | Implement snapshot deletion with safe chunk deletion using reference counts | Abdul Rehman |
| BV-14 | Display backup progress and status | Moiz |
| BV-15 | Implement error handling and logging | All |

## PRIORITY 3 - MEDIUM (Additional Features)

| ID | Backlog Item | Assigned To |
|---|---|---|
| BV-16 | Implement scheduled backups | Qasim |
| BV-17 | Implement backup search and filtering | Moiz |
| BV-18 | Add backup destination configuration | Abdul Rehman |
| BV-19 | Implement backup size calculation (original size vs stored size) | Abdul Rehman |
| BV-20 | Add storage usage visualization | Moiz |
| BV-21 | Implement snapshot naming and organization | Qasim |
| BV-33 | Implement snapshot comparison | Qasim |
| BV-34 | Build snapshot comparison screen | Moiz |

## PRIORITY 4 - LOW (Polishing and Finalization)

| ID | Backlog Item | Assigned To |
|---|---|---|
| BV-22 | Improve GUI styling and responsiveness | Moiz |
| BV-23 | Add confirmation dialogs for destructive actions | Moiz |
| BV-24 | Optimize backup and restoration performance | Qasim |
| BV-25 | Perform integration testing | All |
| BV-26 | Fix bugs discovered during testing | All |
| BV-27 | Prepare project documentation | All |
| BV-28 | Prepare final demonstration and presentation | All |
