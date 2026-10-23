# Release staging record, 2026-10-19

Historical files from the first public-release staging. They are kept for the
record and are **not** needed to build or test. Paths inside them refer to the
original staging layout (files at the repo root), so `SHA256SUMS.txt` and the
`prep-windows` scripts will not match this tree as-is.

- `MANIFEST.md`, `SHA256SUMS.txt`: what the staging contained, with hashes.
- `PUBLISH-GUIDE.md`, `prep-windows.bat/.ps1`: the one-time publish steps.
- `LICENSE-OPTIONS.md`: the license decision record (MIT chosen 2026-10-19).
- `pch-release-package/`: the standalone PCH package as staged. The live copy
  is `pch/` at the repo root.
