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
  Otherwise (or if encoding fails) the chunk is stored raw.
- After decoding, SHA-256 of the result must equal the file name; otherwise the chunk is corrupt.
- Unknown version, unknown method, bad magic, impossible sizes, truncated header or payload → error.
- Files are written through a temp file + rename (`QSaveFile`) so a crash leaves no half-written chunk.

## Huffman payload (method 1, BV-09)

Canonical Huffman coding, implemented in `compression/Huffman`. The payload is:

| Offset in payload | Size | Field |
|---|---|---|
| 0 | 256 | code length in bits for each byte value 0..255 (`0` = value does not occur) |
| 256 | n | the codes of all original bytes, packed most significant bit first; unused bits in the last byte are `0` |

- Codes are rebuilt from the lengths alone: shorter codes first, equal lengths in byte-value
  order. No tree is stored.
- The number of decoded bytes is the header's **original size**; it is not repeated in the payload.
- Code length is at most 32 bits. One distinct byte value gets a 1-bit code (`0`).
- Decoding fails if the length table is not a valid prefix code (the codes must fill the code
  space exactly; a single symbol must have length 1), the bit stream runs out before
  *original size* bytes are decoded, extra bytes follow the last code, or the padding bits are not zero.
- The fixed 256-byte table makes Huffman a loss for small or high-entropy chunks, which is why
  `ChunkStorage` compares sizes and falls back to raw.
