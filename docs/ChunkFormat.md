# BackupVault — Chunk File Format v1 (BV-05)

**Status:** Proposed by Abdul (ChunkStorage owner). Qasim to review (decision D6).

One file per unique chunk: `<VaultRoot>/chunks/<64-char lowercase hex SHA-256 of the raw chunk>`.
All integers are **little-endian**. Header size is **14 bytes**.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | ASCII `BVCK` |
| 4 | 1 | version | `1` |
| 5 | 1 | method | `0` = raw, `1` = Huffman |
| 6 | 4 | original size | uncompressed byte count (≤ 65,536) |
| 10 | 4 | payload size | number of payload bytes that follow |
| 14 | n | payload | raw bytes, or Huffman-encoded bytes |

## Rules

- File size must equal `14 + payload size` exactly; anything else is an error.
- Raw method: payload size must equal original size.
- Huffman is used only when header + encoded payload is smaller than header + raw bytes.
  (Not implemented yet: BV-09. Until then every chunk is stored raw, and loading a
  Huffman chunk returns an error.)
- After decoding, SHA-256 of the result must equal the file name; otherwise the chunk is corrupt.
- Unknown version, unknown method, bad magic, impossible sizes, truncated header or payload → error.
- Files are written through a temp file + rename (`QSaveFile`) so a crash leaves no half-written chunk.
