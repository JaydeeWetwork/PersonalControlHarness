# Release staging record, 2026-10-19

Historical files from the first public-release staging. They are kept for the
record and are **not** needed to build or test. Paths inside them refer to the
original staging layout (files at the repo root).

- `MANIFEST.md`: what the staging contained.
- `PUBLISH-GUIDE.md`: the one-time publish steps.
- `LICENSE-OPTIONS.md`: the license decision record (MIT chosen 2026-10-19).

The staging checksums (`SHA256SUMS.txt`), the `prep-windows` scripts and the
duplicate `pch-release-package/` copy were removed on 2026-10-23 because they
no longer matched this tree. They remain in git history. The live PCH package
is `pch/` at the repo root.
