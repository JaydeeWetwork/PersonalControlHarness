# Release sign-off: PCH 0.0.1.0

Release 0.0.1.0 (no pre-release suffix), the version named by the DeepSeek
model's second external review (2026-10-23). No git tag is created.

## What was executed (2026-10-23, America/New_York)

| Machine | Commit tested | Command | Result |
|---|---|---|---|
| Windows 11 workstation (Intel i5-12450H, VS 2026 Build Tools, Python 3.14.8, torch 2.14.1+cpu) | `86f1c2e` | `cmake --preset vs2026`; `cmake --build out\build\vs2026 --config Release -j 8` | build OK |
| same | `86f1c2e` | `ctest --preset vs2026-release -j 8 --repeat until-fail:3` (`PCH_SPECIMEN_DIR` set to an external DeepSeek-V3 checkout) | 78/78 passed x3; 75 ran, 3 skipped (`engine_storage`, `engine_assets`: no symlink privilege; `pch.weights_local`: no weights) |
| same | `86f1c2e` | same, `PCH_SPECIMEN_DIR` unset (vendored `third_party/DeepSeek-V3`) | **FAILED:** `engine_sim` hung (idle, about 14 min) and was killed; all other tests passed. In isolation, `ctest -R ^engine_sim$ --repeat until-fail:40 --timeout 60` passed 35 times, then timed out on run 36 (B8) |
| same | `816b447` + rewind/fork patch (committed as `86f1c2e`) | `python testbeds/tb_model_testbeds.py rewind_fidelity` / `fork_budget` | ALL PASS; rewind N = 1, 64, 240; fork up to 32 branches, horizon 240; N > 240 NOT RUN |
| Linux box (kernel 6.12, GCC, Unix Makefiles, Python 3.13.5, no torch, 8 cores) | `86f1c2e` | `make -j6 && ctest -j8 --repeat until-fail:3` | 78/78 passed x3; 70 ran, 8 skipped (`pch.weights_local` and 7 torch model beds) |
| Linux box | each commit | `python3 tools/check_docs.py`; `tools/check_upstream_drift.py`; `actionlint` on `.github/workflows/*.yml` | PASS; no drift from DeepSeek-V3 `9b4e978`; actionlint clean |

Commits after `86f1c2e` change only documentation, credits, the docs check
and version strings (VERSION, CMakeLists `project()`, CITATION.cff); the
docs check passes on the final commit.

## What is claimed vs. not yet shown

- Claimed and tested: everything marked `verified` in [CLAIMS.md](CLAIMS.md).
- **Planned / known limitations (open at release):**
  - Signed receipt from a real (non-fixture) specimen: no weights downloaded;
    the chosen specimen is Qwen2 and needs a loader (BLOCKERS B3, B4).
  - First green CI run on GitHub: workflows configured and linted, not yet run (B5).
  - Engine demo hash repin (B6): `--demo-test` still fails its pin check.
  - Rewind beyond N = 240 needs a longer-context fixture; real-model N = 64 waits for the specimen.
  - **`engine_sim` intermittent hang (B8):** blocks the push condition "all tests pass"; not a PCH-harness test, it lives in the LithoLattice engine.
  - Beds 08-14 run on the reference boss stub, not the LithoKraft boss (B2).

## Sign-off

JaydeeWetwork, 2026-10-23, approved release 0.0.1.0 (with the open items above listed as known limitations).
