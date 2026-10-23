# Staging Manifest — Combined Release (UNPUBLISHED)

Assembled: 2026-10-19, on Elfie's workstation, from the production tree
`~/workspace/litholattice/` and the PCH package `~/workspace/pch-release/`.
**Nothing in this staging has been uploaded, pushed, or shared.** It
exists so JD can inspect exactly what a combined open release would
contain before he decides whether to lift the standing source-code lock.
No production file was modified; this staging is copies only.

## Totals

| Section | Files | Bytes |
|---|---:|---:|
| `engine/` (LithoLattice) | 632 | 6,005,026 |
| `game/` (LithoKraft) | 1,176 | 8,811,935 |
| `pch/` (Personal Control Harness) | 19 | 79,088 |
| `docs/` | 6 | 72,301 |
| `tools/` | 3 | 42,077 |
| `demo_record_proof.lldemo` | 1 | 398 |
| Authored for this staging (`README.md`, `LICENSE-OPTIONS.md`, this file) | 3 | — |
| **Total** | **1,840** | **≈15.0 MB** |

Source volume: engine C++ 99,640 lines (492 `.cpp`/`.h` files);
game C++ 36,195 lines (194 files); 135,835 lines total.
Copy fidelity was cross-checked: file counts in staging match the
production source areas exactly (engine 632, game 1,176).

Structure preserved from production, including the symlink
`engine/LithoKraft -> ../game/LithoKraft` the build relies on.

## Excluded from production, with reasons

**Build output and caches (≈1.7 GB of the 1.7 GB production tree):**

- `build-debug/` (1.2 GB), `build-linux/` (278 MB), `engine/build/`
  (227 MB) — CMake build trees and fetched dependencies. Rebuilt from
  source by anyone; never shipped.
- `Testing/` — CTest scratch output.
- `tools/__pycache__/`, `web-spike/__pycache__/` — Python bytecode
  caches.

**Redundant archives (the loose source ships instead):**

- `litholattice-full-2026-10-11.zip` (8,583,367 B),
  `LithoKraft-game-src-100ecf3.zip` (3,201,801 B),
  `LithoLattice-engine-src-100ecf3.zip` (1,196,355 B) — dated snapshot
  archives of this same source, kept in production only.

**Scaffolding and runtime output:**

- `web-spike/` (2.0 MB) — browser-spike experiments, including built
  `.wasm`/`.js` artifacts and the tree's only `*.orig-*` backup files
  (7). Superseded by the full browser test; not part of the product.
- `game/LithoKraft/logs/` (2 files) — runtime logs; output, not source,
  and can contain local machine paths.
- `demo_sw.cpp` (production root) — standalone scratch demo, not wired
  into any build.

**Internal documents withheld (production root + `docs/`):**

- `00-README-HANDOFF.md` and its identical copy `docs/00-README-HANDOFF.md`
  — internal handoff memo, self-marked "CONFIDENTIAL, JD and Elfie only."
- `docs/master-builder-brief-2026-10-10.md` — internal squad brief;
  contains JD's privately held domain names and internal role
  assignments.
- `docs/DESIGN-BRIEF-full.md` — internal working brief (production
  schedule notes, squad references).
- `docs/LithoKraft-Mod-Developer-Architecture-for-Elfie.md`,
  `docs/LithoKraft-Night-Update-for-Elfie.md`,
  `docs/LithoKraft-Trace-and-Components-for-Elfie.md` — internal squad
  correspondence.
- `BUILD-SETUP-JD.md` (production root) — JD's personal workstation
  setup note; its build instructions are carried by the staging
  `README.md` instead. Contains no secrets.

Any withheld document can be restored to the staging with one word
from JD; they are listed here so the withholding is itself on record.

## Security and privacy scan

Production source areas were scanned before copying, and the finished
staging was scanned again after copying: secret-shaped filenames
(`*.pem`, `*.key`, `*.p12`, `*secret*`, `*credential*`, `.env*`,
`id_rsa*`, `*signing*`), PEM private-key blocks in file contents,
privately held domain names, JD's email address, and family / health /
finance markers.

- **No private key material, credential files, or `.env` files exist in
  the production source tree, and none were copied.** (The `signing.*`
  sources are code that *implements* signing; key files are generated
  by the tooling at runtime and were never in the tree.)
- The only email addresses in the staging are public third-party
  license attributions (`engine/THIRD_PARTY_LICENSES.md`, vendored
  nanosvg headers).
- After the exclusions above, **zero** references to the privately held
  domains remain in the staging (verified by scan).

> ### ⚠ Flag for JD — baked test key (public value, decision pending)
> `engine/src/assets/mod_signature.cpp` carries `kBakedPublicKeyHex`:
> the **retired throwaway test *public* key** for mod-signature
> verification. Its header
> (`engine/src/assets/include/litholattice/assets/mod_signature.h`)
> documents it in-code: "REPLACE BEFORE BETA — this is a throwaway
> test key, NOT JD's real key. The private key never ships." A public
> key is not a secret, and the file is required for the tree to build
> as designed, so it is staged verbatim — but the dev signing key
> decision already on JD's queue (HIGH) should be resolved before any
> publication, so no build ever ships trusting a retired test key.

## License state

**DECIDED (JD, 2026-10-19): MIT.** The root `LICENSE` carries the MIT
text (Copyright (c) 2026 JaydeeWetwork / Wet Wafer Works Games
Incorporated), matching `pch/LICENSE` — one license at the root governs
the whole repo. `LICENSE-OPTIONS.md` is retained as the decision
record. The license is chosen; publication remains a separate pending
decision (the staging is not in force as a release until JD ratifies
publication).

## SHA-256 of key artifacts

```
0d40691bafae593593a30ac02f5584b9037dbd7f043ea5bc7c4466316d16099a  demo_record_proof.lldemo
af1c5b77b6da59177b8a58e64ccae901471f2e48c668ef9376277c40fd53e977  README.md
33e89f1395814e4a7e0802116533c535b40838291b33b485c5b3f20aa1cd1064  LICENSE-OPTIONS.md
90376097fdd7caf243fca880365d22923639ea0443e82e4eda347e5f4a28460b  pch/harness/stream_sampled.jsonl
753cc6dac8d0dc8eb151db98671b3ea3157c2625de45b44813c84e737603cd70  pch/harness/stream_greedy.jsonl
b6886727d950f1cf075f28b8cb2d78da7a7413cab5cc5216b43cd5543fa8165b  pch/harness/stepstream.py
8d16660138f21b65169b0627d807c6f2ea9ca52bb8ebd3ba3e6a71c3c68b93e4  pch/harness/harness.py
0982e82513f078327bc9f31d95b5002fe4fdff10ee54510d4b664e87829752ec  game/LithoKraft/CREDITS.md
95b30033a71259047a88e0fed4a22610c85e47a4d46ffeb32ddce43fe549ddae  engine/CMakeLists.txt
6e89990afc850ecf29afc830836c0e0d4f36a82a6225166daafc9ecceac40d00  game/LithoKraft/CMakeLists.txt
eb478f92c61c1917d7791b6b08dc46ce28755bd709c740c185decced942e3cf4  engine/README.md
43703a3c427d4adfec6d630d51d4c18d8d9a43367d0ddae3f5f394e346f4e298  engine/THIRD_PARTY_LICENSES.md
```
