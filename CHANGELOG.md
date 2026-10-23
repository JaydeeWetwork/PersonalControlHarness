# Changelog

Versions use a four-place rolling counter (`a.b.c.d`).

## 0.0.1.0 (2026-10-23)

**Release** (no pre-release suffix), approved by JaydeeWetwork; version named
by the DeepSeek model's second external review. Round 2:

- **CI:** `.github/workflows/ci.yml` (Windows and Linux, Release and Debug,
  Python 3.10 and 3.12) and `nightly.yml`; SHA-pinned actions, read-only
  permissions, actionlint clean. First green run pending push.
- **Tests:** `pch.boss.stub_conformance` (contract C1-C8 with mutants,
  `PCH_BOSS_IMPL` for the real boss), `pch.upstream_drift` (DeepSeek-V3
  `9b4e978`), `pch.key_rotation` (rotation, revocation, back-dating,
  passphrase). Rewind to N = 240 and fork horizon 240, the toy fixture's
  ceiling.
- **Docs:** README results refreshed (Windows 78/78 x3, Linux 78/78 x3),
  `docs/RELEASE-SIGNOFF.md`, consortium credits.
- **Known limitations:** real-specimen signed run, first green CI run, demo
  hash repin (see `docs/BLOCKERS.md`).

### Also in 0.0.1.0: media (from the unreleased pch-hardening work)
- **docs/media:** the 15 fps screen captures are replaced with frame-perfect 60 fps renders (boss HUD replay, ctest replay, LithoKraft flyover). They are rendered offline from deterministic sources by `tools/media/make_media.py`, with a new `flyover_capture` tool. README previews are looping WebP. No Git LFS is needed (largest file 4.4 MB).

## 0.0.0.4-alpha (2026-10-23, unreleased; local branch pch-hardening)

**Alpha.** Hardening phase 1 from DeepSeek's production-readiness plan,
approved by JaydeeWetwork (Ed25519 chosen by JD).

### Section 1: receipt signing (S1.1-S1.8)

- **Trailer in chain (S1.3):** streams close with a schema-2 trailer that is
  itself a chained record; `stepstream.check_sealed` is the core check and
  `harness.verify_replay` uses it. Legacy trailers are labeled
  `legacy-unchained`. Strict JSON loading (no duplicate keys, no NaN).
- **Signatures (S1.1, S1.2, S1.4):** Ed25519 via `cryptography` (OpenSSL 3+),
  one signature per receipt at close, `.sig` sidecar, byte-exact payload with
  golden vectors (`pch/testdata/sig_vectors.json`, RFC 8032 public test key).
- **Tools (S1.5, S1.6):** `pch/tools/pch_verify.py` (offline, exit 0/1),
  `pch_sign.py`, `pch_keygen.py`; `pch/docs/KEY-MANAGEMENT.md`.
- **Tests (S1.7, S1.8):** bed 02 case 3 un-skipped; new bed 15 signature fuzz
  (10,000 mutations), which found and fixed a sidecar hex-encoding
  malleability. New CTest entries: `pch.trailer_in_chain`,
  `pch.sig_golden_vectors`, `pch.sign_at_close`, `pch.verify_cli_offline`,
  `pch.signature_fuzz`. Fail-before/pass-after record:
  `docs/hardening/FAIL-BEFORE.md`.
- `cryptography>=44` added to `requirements.txt`.

### Vendored specimen code (E3.0, JD addition)

- DeepSeek-V3 inference code (code only, no weights) vendored unmodified at
  `third_party/DeepSeek-V3/` from upstream `9b4e978`, with `LICENSE-CODE`
  (MIT), attribution and a sha256 manifest (`UPSTREAM.md`) checked by
  `pch.vendored_specimen`. `PCH_SPECIMEN_DIR` still overrides it.

### Local model weights (JD request)

- `pch/harness/weights.py` and `pch/tools/pch_weights.py`: `--weights`,
  `PCH_WEIGHTS_DIR`, `"weights_dir"` in `pch.config.json`, default
  `models/weights/`, in that order; clear errors naming the expected files.
  Nothing is downloaded. Tests: `pch.weights_path`, `pch.weights_local`.

### Section 2: test beds (T2.1, T2.2)

- `testbeds/DECISIONS.md`: owner decision (JD) to keep and implement all 14.
- Beds 10, 12, 13, 14 now run on a minimal PCH reference boss stub
  (`pch/harness/boss.py`: brain stamp, rolling counter, rate cap, locked
  branch count, refuse-or-label replay) via `testbeds/tb_boss_fixture.py`.
  Beds 08, 09, 11 remain skipped with reasons (`docs/BLOCKERS.md`).

### Section 4: docs-to-reality sync (D4.1-D4.6)

- `docs/pins.json` lists every pin. The game demo test repin (D4.1) was
  reverted after a build measured `aa82e7afb1eff9d3`; open for JD
  (`docs/BLOCKERS.md` B6).
- `docs/CLAIMS.md` claims register; README labels unbuilt items `Planned:`.
- Credit wording: the AI squad designed the test suites without JD's input;
  JD human-reviews, verifies and approves everything before posting.
- `tools/check_docs.py` as ctest `pch.docs_check`; CONTRIBUTING rule that a
  claim change updates CLAIMS.md and CHANGELOG.md.

## 0.0.0.3-alpha (2026-10-23)

**Alpha.** Credits release; no code or test changes.

- Includes both credit updates: PCH originally designed by Elfie Muse AI,
  with credit to the Muse team (the team that created the system Elfie is
  built on); and "Timing handshake, LithoLattice engine and LithoKraft game:
  original algorithms designed by JaydeeWetwork (open source under MIT)."
  The second line was committed after the 0.0.0.2-alpha tag, so this is the
  first tag carrying both.

## 0.0.0.2-alpha (2026-10-23)

**Alpha.** Credits correction only; no code or test changes.

- PCH originally designed by Elfie Muse AI; credit added for Elfie Muse AI
  and the Muse team (the team that created the system Elfie is built on)
  in README, CREDITS.md and CITATION.cff.
- Credit line added in README and CREDITS.md: "Timing handshake, LithoLattice engine and LithoKraft game: original algorithms designed by JaydeeWetwork (open source under MIT)."

## 0.0.0.1-alpha (2026-10-23)

**Alpha.** First tagged pre-release. Interfaces, file formats and results may
change without notice; not for production use.

- **Test beds:** fourteen PCH test beds (`testbeds/`), each designed
  independently by a squad member (Cortana, CloudAtlas, Herominde,
  Master Chief) and built by Cortana. 01-07 ran and passed (06 only at
  N = 1 and 64; larger N exceeds the toy model's 248-step context).
  08-14 target boss features that are not built yet and are not run.
- **Build:** CMake presets for Visual Studio 2026 (`vs2026`, `vs2026-release`),
  usable from VS Code, Visual Studio or the command line with free tools.
- **Tests:** full `ctest` run: 65/65 with 0 failed (56 passed, 9 skipped).
  Skips: `engine_storage` and `engine_assets` need symlink rights
  (Windows Developer Mode); the seven `pch.boss.*` tests await unbuilt
  features.
- **docs/media:** real screen captures of the test beds in a terminal and
  of LithoKraft (Debug build) as MP4 and GIF.
- **Docs and credits:** landing-page README, per-test-bed credits
  ("Designed by" / "Built by: Cortana"), CREDITS.md and the DeepSeek-V3
  acknowledgement. (Credit for Elfie Muse AI as the PCH's original
  designer, and for the Muse team, was added in 0.0.0.2-alpha.)

Known issues: the core receipt trailer-hash gap is documented and not yet
closed; the LithoKraft clip shows a Debug build because the Release build
refuses to boot with the retired test key.
