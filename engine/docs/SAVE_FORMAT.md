# LithoLattice world save format (v1)

Our own format (no LevelDB). The engine module `litholattice::storage` owns the container and the
block codec. The game registers its own payloads (SDK style). Status of each item is marked
**[v1]** (implemented and tested in `tests/` `storage`) or **[planned]**.

## World directory
`<world>/r.<rx>.<rz>.llr`, one region file per 32 x 32 chunk columns **[v1]**. Region coordinates are
int32 and file names are created only for regions that hold data, so addressing is sparse and
effectively unlimited (about 2^36 blocks a side). The test saves columns 2 billion blocks out.
`<world>/level.json` — world metadata **[v1]**: `{ "saveFormatVersion": 1, "seed": "<decimal uint64>" }`, written through the file contract (`writeFileAtomic`, fsync + backup rotation) on every save and read back before dimension creation on startup; the persisted seed wins over `--seed`/the default, and an explicit `--seed` that disagrees logs a loud warning. The seed is a decimal STRING because the engine JSON number type is a double and cannot hold every uint64 seed exactly. A save with no level.json is a pre-metadata save: the caller's seed stands. (Dimension list / further game metadata: still planned.)

## Region file (little-endian)
| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `LLGR` (0x52474C4C) |
| 4 | 2 | region format version (1) |
| 6 | 2 | reserved |
| 8 | 1024 x 16 | index: per column slot (`lz * 32 + lx`) `u64 offset, u32 size, u32 crc32`; size 0 = absent |
| 16392 | ... | chunk records |

Random access: read the 16 KB index, then seek straight to one record. **[v1]** (v1 reads the whole region on first touch and keeps it cached.)

**Crash safety [v1]:** a save writes the whole region to `r.x.z.llr.tmp`, flushes it, then renames it
over the old file. The rename is atomic on one volume, so a crash leaves either the old region or the
new one, never a torn one. A region whose header or index is unreadable is never overwritten: it is
kept for recovery and its saves are refused with an error. The whole write path goes through the
engine file contract (`storage/file_contract.h`): temp file + fsync (+ parent-directory fsync on Linux)
+ atomic rename, with **8 last-known-good backups** (`<file>.bak.1` .. `<file>.bak.8`, the contract's
default depth `kBackupDepthDefault`) rotated on each successful write. The one crash window in that
sequence — between backup rotation and the commit rename, which leaves the target absent — self-heals
on the next open via `recoverCrashedWrite()`: the newest intact backup generation (or a fully-written
temp file) is promoted, and a torn primary is moved aside to `<file>.torn` (rotated to `.torn.1`,
`.torn.2`, ... when one already exists, so the first torn evidence is never overwritten). A region
that fails validation on open is quarantined: skipped with a logged error and counted
(`WorldStore::quarantinedCount()`), never crashing the load. Corrupt chunk records fail their CRC and
read absent. Region files are also size-capped on open (64 MiB, the file contract's whole-file read
cap): a larger file is refused before it is read into memory. **[planned]:** append-in-place with a
small write-ahead journal so one column does not rewrite 1 MB.

## Chunk record
`u16 recordVersion (1), i32 cx, i32 cz, u16 payloadCount`, then per payload
`u32 tag (fourcc), u16 payloadVersion, u32 length, bytes`. CRC-32 (IEEE) of the whole record sits in
the region index; a mismatch reports "CRC mismatch" and that column is skipped. **[v1]**

**CRC32 is corruption detection, not authenticity (C-I2):** the CRC is an unkeyed checksum. It
detects torn writes, bit rot and accidental damage — it proves NOTHING about who wrote the file,
and anyone can recompute a valid CRC over modified data. Never treat a CRC-valid save as trusted
input beyond its structural checks; authenticity, where it is ever needed, requires a keyed MAC /
signature layer that this format does not have.

## Payloads and versioning
- `BLKS` v1 (engine): `u64 presentMask` (bit per non-empty sub-chunk of 48), then per present
  sub-chunk the block ids and the cell states, each **palette + bit-packed** (u16 palette size,
  palette entries, then `ceil(log2 n)`-bit indices in u64 words; a uniform sub-chunk takes 4 bytes).
  The cell states carry LithoKraft's per-cell signal state (value + facing). **[v1]**
- `MATS` v1 (engine, PBR phase 2): the per-voxel 8-bit material bytes (`SubChunk::materials`) plus
  the per-column material string-ID table. Saves persist **string IDs, never compact IDs**. Layout:
  `u16 tableSize (1..256)`, then per entry `u16 len, len bytes` (string IDs in voxel-index order),
  `u64 presentMask` (same convention as BLKS), then per present sub-chunk the material bytes,
  palette + bit-packed (u16 entry count, u8 entries, `ceil(log2 n)`-bit indices; a uniform
  sub-chunk takes 3 bytes). Load rules: `tableSize > 256` is an error (never silent truncation);
  unknown string IDs resolve to the defined fallback `litholattice:default` (never garbage);
  a MATS sub-chunk with no BLKS counterpart is corrupt. A separate tag (not a BLKS version bump),
  so old saves without MATS keep loading with the default palette. No migration chain. **[v1]**
- Game payloads: `PayloadRegistry::add(PayloadType{tag, version, save, load, migrate})`. On load,
  an older payload goes through `migrate[v] : v -> v+1` until it reaches the current version. A
  payload newer than the build is refused, never misread. Unknown tags are skipped. **[v1]**
- `SGNL` v2 (LithoKraft, `SignalSim::kStateVersion` = 2): per component in the column, sorted by
  position: `u64 packPos, u8 out, out2, pulse, prevIn, prevSide, prevClk, history, timer, channel,
  beacon, u16 nKeys, (u8 front, u8 control) x nKeys`. (v1 lacked `channel`/`beacon`; the registered
  migration inserts channel=1, beacon=0.) On load, `scan()` re-indexes components from the blocks and
  `importState()` restores their memory. Wire strengths and torch / lamp states are cell states in
  `BLKS`. **[v1]**
- `<root>/player.dat` (LithoKraft) **v4** (current; v1–v3 still load): file = `u32 "LLPM" magic,
  u32 crc32(body), u32 bodyLen, body`. Body: `u16 version (4), f32 x y z yaw pitch, string dimension`
  (u32 length + bytes, like every string here), `u8 hot slot, u8 hotbarCount`, then the hotbar —
  in v4 one **string id per slot** ("" = empty; name-remapped on load through blocks.names, aliases
  included; an unknown name loads as an empty slot, logged), in v1–v3 `u16` raw BlockIds instead.
  Then, when the version carries them: the **v2 stat block** (`u16 count`, per entry `string name,
  u32 value` — int32 bit pattern), the **v3 vitals block** (same encoding), and the **v4 inventory
  block** (`u16 slotCount`, per slot in `Inventory::allSlots` order `string itemId, u16 count`;
  empty slots are explicit "" + 0; an unknown item id drops only its own stack, logged). A v1 file
  has none of the three blocks; a pre-v4 file loads with an empty inventory. Reading requires the
  body to be consumed EXACTLY (trailing bytes are refused) and caps the file at 1 MiB. Written
  through the file contract (`writeFileAtomic`: fsync + backup rotation, depth 8), and read only
  after `recoverCrashedWrite` with a magic+CRC validator, so a torn player.dat heals from its
  newest intact backup. **[v1]**
- Entities: none exist in the game yet; `ENTS` payload **[planned]** when they do.

### LithoKraft wiring [v1]
Saves live in `<data root>/saves/world/<dimension>/` (`--save-dir`, `--no-save`; `--bench` never
saves). Startup loads the player and its dimension. Each generated column with a saved copy is
replaced by it (loads stream on the I/O thread), then the signal memory is imported. Saves happen
on F5 (Debug context, was free), every 5 minutes (autosave), on dimension switch and on exit
(exit waits for the disk). A save stops the sim thread for the snapshot: the columns and signal
state are encoded on the main thread, and the files are written on the I/O thread.

## Compression
v1 relies on the palette + bit-packing (2 typical sub-chunks: under 6 KB instead of 32 KB raw).
**[planned]:** per-record LZ4 (BSD-2-Clause, fast load) or zstd (BSD-3-Clause / GPL-2.0 dual,
used under BSD) behind a codec byte in the record header. Both licenses are commercial-friendly.
No compression library is linked yet.

## Threading
`WorldStore` runs all file I/O on its own thread. `saveAsync(column)` encodes the column on the
caller's thread (a snapshot, so the caller may keep editing), and the I/O thread writes it.
`loadAsync(cx, cz, callback)` decodes on the I/O thread. `flush()` blocks until every region is
swapped in. **[v1]**
