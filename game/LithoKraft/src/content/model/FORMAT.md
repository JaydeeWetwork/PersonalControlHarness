# .lthm — LithoKraft model file format

Version 1 (2026-10-11, workstream 3). The on-disk format for saved
subdivision models: subdivide → add detail → save → place.

## Design goals

- **World permanence:** models saved today must load in future engine
  versions. The format is versioned from day one; readers reject unknown
  versions with a clean error instead of guessing.
- **Deterministic:** the same authoring calls always produce byte-identical
  files (leaf order is fixed, integers are little-endian, no timestamps or
  padding in the payload).
- **Corruption-proof:** a CRC32 over the payload catches bit rot and
  truncated downloads before any field is trusted.

## Layout

All integers are little-endian. Offsets are from the start of the file.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"LTHM"` (0x4C 0x54 0x48 0x4D) |
| 4 | 2 | version | u16, currently `1` |
| 6 | 2 | flags | u16, reserved; writers set `0`, readers ignore |
| 8 | 2+N | name | u16 byte-length N, then N bytes of UTF-8 (no NUL) |
| … | 2+M | author | u16 byte-length M, then M bytes of UTF-8 (may be empty) |
| … | 1 | maxLevel | u8, LOD hint: highest level carrying useful detail (0–6) |
| … | 4 | leafCount | u32, number of leaf cells that follow |
| … | 5×leafCount | leaves | per cell: `level` u8, `x` u8, `y` u8, `z` u8, `material` u8 |
| … | 4 | crc32 | u32, CRC32 (IEEE 802.3) of **all preceding bytes** |

Minimum valid file: 12 bytes (empty name/author, zero leaves) + 4 CRC = 16.

### Leaves

Each leaf is one `MicroCell` from `SparseOctree::leaves()`: `(level, x, y, z)`
with `level` in [0, 6], `x/y/z` in [0, 2^level), and `material` nonzero (0 =
empty is never stored). Leaves are written in the deterministic order
`leaves()` returns: by level, then z, then y, then x.

## Reader contract

Load in this order; each step fails cleanly with a `LoadError`, never a crash:

1. Need ≥ 12 bytes or the file is **truncated** (≥ 4 bytes with wrong magic
   is **bad magic**).
2. Magic must be `"LTHM"` → else **bad magic**.
3. Version must equal the reader's supported version → else
   **unsupported version**, with a message naming the found version
   (e.g. `"unsupported .lthm version 2 (this build reads version 1)"`).
   Version is checked **before** the CRC so a future file fails fast with
   the right error even if nothing else parses.
4. Structural parse with bounds checks → short reads are **truncated**;
   out-of-range level/coords, zero-material leaves, or bad string lengths
   are **bad data**.
5. CRC32 over everything except the last 4 bytes must match → else
   **bad CRC** (corrupt). The octree is only built after the CRC passes, so
   a truncated download reports **truncated** while a bit-flip in an
   intact file reports **bad CRC**.

## Versioning policy

- **Major layout changes** bump `version`. Old readers reject new files
  (unsupported version); new readers may keep loaders for old versions.
- **Minor additions** use the `flags` field or append-only trailing sections;
  v1 readers ignore unknown flags.
- Never reinterpret a field's meaning under the same version.

## Notes for authors

- `maxLevel` is a hint for LOD selection, not a constraint: a level-6 model
  placed in a low-detail context may render coarser, but the saved detail is
  never discarded.
- Material indices are a mesher passthrough; their meaning lives with the
  block/material registry, not the file.
- File naming convention in `assets/models/`: `<name>.lthm`, lowercase,
  hyphens for spaces (e.g. `silicon-pine.lthm`).
