# Architecture

## Backup workflow
1. GUI (Moiz) receives the source folder and starts a backup.
2. Backup Engine (Qasim) scans the folder, splits files into 64 KB chunks and hashes each chunk.
3. ChunkIndex (Hash Table) checks whether the chunk already exists.
4. New chunk: Huffman compresses it, ChunkStorage writes it (Abdul).
5. DatabaseManager records chunk metadata, file/chunk relations and the snapshot (Abdul).
6. GUI shows the snapshot and status.

## Restore workflow
1. GUI selects a snapshot and a target folder.
2. Restore Engine gets file and chunk ordering.
3. Storage reads the chunks and Huffman decompresses them.
4. Files are rebuilt in chunk order and verified byte-for-byte.
