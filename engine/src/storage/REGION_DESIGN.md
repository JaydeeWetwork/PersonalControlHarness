# Anvil-Inverted Region Layer — Design Document

Engine-level storage primitive (`litholattice::storage::AnvilRegion`, see
`include/litholattice/storage/anvil_region.h`). Groups 32x32 chunk columns per
file for locality, learning from Minecraft's Anvil `.mca` region format but
**inverting its crash-safety weakness**: Anvil has no journaling and no
per-record checksums (the entire third-party region-repair-tool ecosystem exists
to clean up after it). This layer adds what Anvil lacks — per-record CRC32 plus
a payload-first / header-second commit protocol — so a torn write is always
detectable and never points at garbage.

Payloads are **opaque bytes + a 1-byte codec tag**: the layer never interprets
chunk contents (no game block types in the API). It sits *around* the existing
SGNL v2 save machinery as an engine primitive; it does not replace the save
format.

## On-disk layout

```
offset   size     what
0        16       file preamble: [magic u32 LE "LLAR"][format version u16 LE = 1]
                                  [sector size u16 LE = 512][reserved u64 = 0]
16       4096     location table: 1024 entries x 4 bytes
4112     4096     timestamp table: 1024 x u32 LE dirty-generation counters
8208     496      padding to 17 x 512-byte sectors (first data sector = 17)
8704+    ...      records, allocated in 512-byte sector runs
```

### Location entry (4 bytes — Anvil-compatible encoding)

Entry for local chunk `(lx, lz)` is at byte `16 + 4 * (lz * 32 + lx)`:

- bytes 0–2: **big-endian** sector offset from the start of the file, in
  512-byte sector units (deliberately Anvil's exact 3-byte BE + 1-byte count
  encoding, so the format reads as "Anvil with smaller teeth").
- byte 3: sector count the record occupies (0 = absent; never-written chunks
  cost zero bytes beyond the header slot). The top bit (`0x80`) is the
  **external flag**: the record lives in a sidecar file, not in sectors.

Sectors 0–15 are reserved (header). First data sector is 16.

### Record layout (inline, at `offset * 512`)

```
[u32 LE payloadLen][u8 codec][u8 flags][payload bytes...][u32 LE crc32][pad to 512 multiple]
```

`crc32` covers `payloadLen || codec || flags || payload` — it sits at the end so
the covered bytes are contiguous. The 1-byte **codec tag is opaque** to the layer; the Anvil
numbering is adopted by convention (`3` = none, `4` = LZ4, `127` = custom) so
future codec experiments never touch the container. `flags` is reserved (= 0).

A record must satisfy `10 + payloadLen <= sectors * 512`. The 1-byte sector count
caps inline records at `255 * 512 - 10 = 130550` bytes (~127.5 KiB).

### Overflow sidecars

Records exceeding the inline cap spill to a sidecar file in the same directory,
named `<region-stem>.c<x>.<z>.llx` (absolute chunk coordinates, mirroring
Anvil's `c.<x>.<z>.mcc` convention). The sidecar holds the raw record bytes
(`[len][codec][flags][crc][payload]`, file size is not the length — the length
field is). The index entry is a fixed-width stub: `{offset: 0, count: 0x80}`.
The sidecar is written and flushed **before** the index entry is committed, so
the same payload-first ordering protects the external path.

### Timestamp table as dirty-generation vector

The second 4 KiB table stores one u32 per slot: a **monotonic write generation**,
not wall-clock time. Each `put` bumps the slot's generation; on `open()` the
region resumes the counter from the table's maximum, so generations stay
monotonic across restarts. The save scheduler can diff generations to find
*which* chunks changed without deserializing any payload (cheap staleness
check), complementing the engine's existing dirty-flag system (which says *why*
they changed).

## Commit protocol (crash safety — the Anvil inversion)

`put(lx, lz, codec, payload)` is two phases, exposed explicitly as
`stage()` + `commit()` for testability:

1. **Payload first.** Allocate a sector run (in-place rewrite if the new record
   still fits the old run; otherwise first-fit from the rebuilt free bitmap, or
   append at end-of-file), write the full record bytes, flush.
2. **Header second — the single commit point.** Write the timestamp-table entry,
   then the 4-byte location-table entry **last**. Flush.

Crash cases, all safe by construction:

- **Crash during payload write:** the index still points at the old record (or
  is all-zero for a new chunk). The half-written bytes sit in sectors the index
  never references; on reopen the free bitmap is rebuilt from the index alone,
  so those sectors are simply marked free and later overwritten. The reader sees
  the old record or "absent" — never garbage.
- **Crash between the two header writes:** the timestamp entry may be newer than
  the location entry. The timestamp table is advisory only; readers take the
  location entry as truth. Result: old data, correct.
- **Crash during the 4-byte entry write:** the entry is 4 bytes; even if it
  tore, every read validates the target record's CRC32, bounds (`offset + count`
  inside the file), and length consistency (`10 + len <= sectors * 512`). A torn
  entry fails validation and the record is reported **corrupt/absent, never
  garbage** — the exact failure mode Anvil cannot distinguish.
- **Crash during `erase`:** erase clears the entry last (freeing is in-memory
  bitmap bookkeeping). A torn erase reads back the old record — stale, never
  garbage.

No journal, no write-ahead log, no persistent allocator state: the free-sector
bitmap is **rebuilt from the location table on every open** (copied from Anvil —
one fewer structure to keep consistent across crashes).

## Crash-recovery procedure

On `open()`:

1. Validate preamble (magic, version, sector size). Failure → `open()` returns
   false; the file is **never overwritten in place** (same rule as the existing
   `RegionFile`: a damaged region is kept for recovery tooling).
2. Load the location table; sanity-check every entry (offset/count within file,
   count byte sane). Entries failing validation are treated as absent —
   individually, without invalidating the region.
3. Load the timestamp table; resume the generation counter from its max.
4. Rebuild the free-sector bitmap from the surviving index entries.

There is no repair pass that mutates the file: recovery is *read-side*. Records
are validated lazily on `read()` via CRC32; corrupt records report an error and
are skipped, neighbours unaffected. A future explicit defragmentation operation
(`recreateRegionFiles`-style) may rewrite the file fresh — out of scope for v1.

## Anvil vs. ours

| Property | Anvil (`.mca`) | This layer (`.llar`) |
|---|---|---|
| Chunk grouping | 32x32 per file | 32x32 per file (same locality win) |
| Header | 8 KiB: 4 KiB location + 4 KiB timestamps | 16-byte preamble + 8 KiB: 4 KiB location + 4 KiB dirty-generation |
| Location entry | 3-byte BE sector offset + 1-byte count | **identical encoding**, sector unit 512 B (vs 4096 B) |
| Sector quantum | 4096 B (~2 KiB avg padding waste/chunk) | **512 B** (~256 B avg waste/chunk — 8x less) |
| Lookup | O(1) via fixed index | O(1) via fixed index |
| Free-space tracking | bitmap rebuilt from header on open | bitmap rebuilt from index on open (copied — no persistent allocator state) |
| Codec agility | 1-byte type tag (3=none, 4=LZ4, 127=custom) | 1-byte opaque codec tag, same numbering |
| Oversize chunks | `& 0x80` external bit → `c.<x>.<z>.mcc` sidecar | `0x80` count-bit → `<stem>.c<x>.<z>.llx` sidecar |
| Per-record checksum | **none** (zlib Adler-32 only for type-2 payloads) | **CRC32 on every record**, verified on read |
| Commit protocol | header updated inline, no ordering guarantee | **payload-first, 4-byte index entry last** as the single commit point |
| Torn-write behavior | garbage / wrong-located chunks / repair tools | old-or-absent, never garbage; unindexed bytes reclaimed as free on reopen |
| Timestamp semantics | wall-clock mtime | monotonic dirty-generation vector for staleness checks |

## API surface

```cpp
litholattice::storage::AnvilRegion region(path);   // path: "<dir>/r.<rx>.<rz>.llar"
region.open(&err);                                  // rebuild bitmap, resume generations
region.put(lx, lz, codec, payload, &err);           // stage + commit, fsync'd
region.read(lx, lz, &err);                         // -> optional<Record{codec, payload, generation}>
region.erase(lx, lz, &err);                        // clear entry, free sectors (+ sidecar)
region.generation(lx, lz);                         // dirty-generation vector read
// two-phase commit, also used by tests to simulate crashes:
auto s = region.stage(lx, lz, codec, payload, &err); // payload bytes only, no index change
region.commit(lx, lz, codec, s, &err);              // index entry last
```

`read()` returns `nullopt` with empty `err` for absent chunks and `nullopt`
with `err` set for corrupt records — callers can distinguish "never generated"
from "damaged".

## Open questions (not guessed)

1. **Journal vs. commit protocol for multi-record atomicity:** the current
   protocol is atomic per record. A save tick that must commit *many* chunks
   atomically (all-or-nothing region flush) would need either a tiny journal of
   pending index updates replayed on open, or acceptance of per-record
   atomicity. Left open pending the save-scheduler design.
2. **Defragmentation:** repeated grow/shrink cycles orphan sector runs (same as
   Anvil); no explicit defrag/rewrite operation exists yet.
3. **Sector quantum tuning:** 512 B is locked for v1; if measured average chunk
   records land far from ~1–8 sectors, revisit (1 KiB quantum is the fallback).
4. **fsync policy:** every `put` fsyncs payload and header (correct but chatty).
   A batched `sync()` / group-commit API for the I/O thread is future work.
