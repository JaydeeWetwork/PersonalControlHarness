# LithoKraft model library

Saved `.lthm` subdivision models live here — one block's worth of authored
micro-cell detail each, ready to stamp into the world with `placeModel`.
Format spec: `../src/content/model/FORMAT.md`.

## Contents

| File | Model | Author | Detail | Notes |
|---|---|---|---|---|
| `bashor.lthm` | BashOR | LithoKraft workstream 5 | L6, 3928 leaves | armory tool |
| `chopor.lthm` | ChopOR | LithoKraft workstream 5 | L6, 2746 leaves | armory tool |
| `heapor.lthm` | HeapOR | LithoKraft workstream 5 | L6, 2096 leaves | armory tool |
| `pickor.lthm` | PickOR | LithoKraft workstream 5 | L6, 2552 leaves | armory tool |
| `seedor.lthm` | SeedOR | LithoKraft workstream 5 | L6, 2070 leaves | armory tool |
| `slicor.lthm` | SlicOR | LithoKraft workstream 5 | L6, 1358 leaves | armory tool |
| `firewall.lthm` | Firewall | items workstream 2026-10-12 | L6, 8640 leaves | shield (item model) |
| `ping.lthm` | Ping | items workstream 2026-10-12 | L6, 1097 leaves | bow (item model) |
| `callback.lthm` | Callback | items workstream 2026-10-12 | L6, 1851 leaves | returning thrown weapon (item model) |
| `debuggor.lthm` | DebuggOR | items workstream 2026-10-12 | L6, 1520 leaves | debug stick (item model) |
| `fragment.lthm` | Fragment | items workstream 2026-10-12 | L6, 17064 leaves | starting book (item model) |
| `sniffor.lthm` | Sniffor | items workstream 2026-10-12 | L6, 5078 leaves | handheld receiver (item model) |
| `atlas.lthm` | Atlas | items workstream 2026-10-12 | L6, 8192 leaves | map tablet (item model) |
| `trace_tree.lthm` | Trace Tree | LithoKraft workstream 5 | L6, 4316 leaves | silicon tree |
| `turtlor.lthm` | TurtlOR | LithoKraft workstream 5 | L6, 7852 leaves | mob |
| `chipor-demo.lthm` | chipor-demo | workstream-4 | L4, 15 leaves | ChipOR viewport demo |
| `clockwood-wirewood.lthm` | Clockwood Wirewood | LithoKraft workstream 3 | L6, 363 leaves | procgen tree family |
| `dopant-crystal-tree.lthm` | Dopant Crystal-Tree | LithoKraft workstream 3 | L6, 1429 leaves | procgen tree family |
| `trace-forest-tree.lthm` | TraceForest Circuit-Tree | LithoKraft workstream 3 | L6, 853 leaves | procgen tree family |

("Detail" = max subdivision level and leaf-cell count, read from the file
headers; all files verified as format v1 with valid CRC32.)

## Conventions

- File name: `<name>.lthm`, lowercase, hyphens for spaces
  (e.g. `silicon-pine.lthm`).
- Every file must load under the current `FORMAT.md` version and round-trip
  through `saveModel`/`loadModel` byte-identically.
- Keep the table above in sync when adding models.
