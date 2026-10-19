# First authored models — workstream 5 (2026-10-11)

The proof of the subdivision pipeline: subdivide -> add detail -> save -> place.
Eight models authored in code through `ModelBuilder` (workstream 3's format),
saved as versioned `.lthm` files under `game/LithoKraft/assets/models/`.

## Files here

| File | Role |
|---|---|
| `model.h` / `model.cpp` | Workstream 3: the `.lthm` format + save/place pipeline |
| `FORMAT.md` | Workstream 3: the on-disk format spec |
| `octree_test_stub.cpp` | Temporary engine `SparseOctree` stand-in (workstream 1 owns the real one) |
| `models.h` / `models.cpp` | **Workstream 5**: the authoring palette + the 8 author functions |
| `author_models.cpp` | **Workstream 5**: the authoring tool (regenerates the `.lthm` files) |
| `authored_models_selftest.cpp` | **Workstream 5**: self-test for the 8 models (ctest: `authored_models_selftest`) |
| `model_selftest.cpp` | Workstream 3: format self-test (ctest: `model_selftest`) |

## The authoring palette

`.lthm` stores materials as opaque uint8 indices (mesher passthrough); the
meaning below is workstream 5's authoring palette. Color binding lives with
the renderer. Ids are stable — never renumber.

| Id | Name | Used for |
|---|---|---|
| 1 | HandleWood | tool handles |
| 2 | HeadMetal | tool heads |
| 3 | BladeEdge | cutting edges |
| 4 | Copper | via-trunk, collars, guards |
| 5 | TraceRed | glowing red trace (tips, inlays) |
| 6 | TraceBlue | glowing blue trace (tips, inlays, eyes) |
| 7 | ShellDark | TurtlOR shell plating, tree substrate |
| 8 | ShellPlate | plastron, neck, baby turtle |
| 9 | SeamRed | shell seam glow, red panels |
| 10 | SeamBlue | shell seam glow, blue panels |

## The models

All authored at level 6 (the full 64^3 micro-detail volume of one block).

| File | Model | Leaves | Look |
|---|---|---|---|
| `bashor.lthm` | BashOR | 3928 | hammer: chunky head, steel striking face, stepped peen, bash-prompt `>_` trace inlay |
| `pickor.lthm` | PickOR | 2552 | pickaxe: head bar with twin downward-curving points, blue-hot tips |
| `chopor.lthm` | ChopOR | 2746 | axe: broad flared blade with bright edge, hammer poll, zigzag inlay |
| `heapor.lthm` | HeapOR | 2096 | shovel: T-grip, cupped scoop, bright digging lip, glowing blue rim |
| `seedor.lthm` | SeedOR | 2070 | hoe: angled copper tines with blue-hot points, floating procgen seeds |
| `slicor.lthm` | SlicOR | 1358 | sword: long blade, glowing fuller, copper guard, red gem pommel |
| `firewall.lthm` | Firewall | 8640 | shield: broad steel plate, copper rim, central boss, red/blue trace-cross inlay |
| `ping.lthm` | Ping | 1097 | bow: stepped curved limbs, copper tips, trace-blue string, nocked packet arrow |
| `callback.lthm` | Callback | 1851 | thrown weapon: three prongs with hot tips, copper return ring on the shaft |
| `debuggor.lthm` | DebuggOR | 1520 | debug stick: wand with steel rotation ring, dial core, red/blue detents |
| `fragment.lthm` | Fragment | 17064 | the starting book: dark bound cover, pale page block, copper clasp, broken trace-square glyph |
| `sniffor.lthm` | Sniffor | 5078 | handheld signal receiver: lit screen, stepped dish, whip antenna |
| `atlas.lthm` | Atlas | 8192 | map tablet: copper frame, pale chart with grid ticks, red/blue route line |
| `trace_tree.lthm` | Trace Tree | 4316 | via-trunk silicon tree: stepped copper column, via collars + glowing via rings, root flare, forking trace branches, alternating red/blue glowing tips |
| `turtlor.lthm` | TurtlOR | 7852 | turtle: domed faceted shell with red/blue Faraday panel seams, plastron, blue-eyed head with antennae, four trace-striped legs, baby turtle rider |

## Regenerating

```sh
# from the build dir:
./author_models [outdir]   # defaults to game/LithoKraft/assets/models
```

Deterministic: re-running produces byte-identical files (checked by the
self-test against the checked-in files).

## Self-test coverage (`authored_models_selftest`)

Per model: save -> load round-trip `modelEquals`; leaf count within
[200, 65536] (sparse — nowhere near a full 64^3 fill); every leaf inside
the volume; bbox non-degenerate; re-author `modelEquals` + byte-identical
files; checked-in `.lthm` loads `Ok` and matches the authoring code
byte-for-byte; all material ids are known palette entries.

## Seam notes

- **Workstream 3** owns `model.h` / `model.cpp` / `FORMAT.md` / `model_selftest.cpp`.
  This workstream codes against that API as landed; no format changes made here.
- **Workstream 1** owns `engine/src/subdiv/octree.h` and the real octree.cpp.
  The header contract changed mid-stream (dtor/copy now declared, not
  `= default`); `octree_test_stub.cpp` was updated accordingly by its owner.
  When the real `octree.cpp` lands, delete the stub and link the engine
  subdiv library instead (both `author_models` and
  `authored_models_selftest` are dev/test targets, never shipping).
- File naming: the task specified `trace_tree.lthm` (underscore); the
  directory README prefers hyphens — the task's explicit names win.
