# Blockers and open items (hardening phase 1, 2026-10-23)

Recorded per the hardening brief. Nothing below is faked; each item is either
a documented skip or not started.

| # | Item | Why it is blocked / not done | Needs |
|---|------|------------------------------|-------|
| B1 | ~~Beds 08, 09, 11~~ | Resolved 2026-10-23: they run on the reference boss stub (HUD feed, wind-up feed, HUD-only path). | - |
| B2 | Beds 08-14 run on the reference boss stub only | `pch/harness/boss.py` is a minimal stub, not the game's boss. Bed 12 does not model inventory-swap timing. | Port the checks to the LithoKraft boss |
| B3 | Section 3: real specimen signed receipt | Weights not downloaded (brief: do not download yet). Waits for the engine fixes to land. | JD go-ahead to download |
| B4 | Specimen architecture mismatch | The chosen candidate, `deepseek-ai/DeepSeek-R1-Distill-Qwen-1.5B` (MIT license, HF revision `ad9f0ae`, one `model.safetensors`, about 3.5 GB, under the 4 GB limit), is a **Qwen2** model (`model_type: qwen2`). The vendored DeepSeek-V3 inference code (`third_party/DeepSeek-V3`) cannot load it. | **JD decision:** (a) add a small Qwen2 loader (e.g. `transformers`, Apache-2.0) for the specimen, or (b) pick a different official model with the V3 architecture (none of the official V3-architecture models is 4 GB or less) |
| B5 | Section 5: CI | No CI runner is configured; nothing was pushed (brief: no pushes). `pch.docs_check` runs in ctest locally. | Choose a CI host; approve workflow file |
| B6 | Engine demo hash pin (D4.1 not completed) | D4.1 repinned to `cb32c824f8b3ea98` per the 2026-10-18 receipts, but building this tree (Release, Windows 11, 2026-10-23) measured `aa82e7afb1eff9d3`, stable across record, no-recorder, replay and 2-worker handshake. That matches neither `cb32c824f8b3ea98` nor the old pin, so the repin was reverted to the original (stale) pin; `--demo-test` still fails on that one check as it did before. The engine fixes in progress elsewhere may move it again. `lithokraft --demo-test` is not a ctest. | **JD decision:** which hash is canonical (adopt `aa82e7afb1eff9d3`, or wait for the engine fixes and re-measure), then register `--demo-test` as a ctest |
| B7 | README results block | The quoted ctest transcript (65 tests) predates 0.0.0.4; the README table vs ctest output is not auto-compared. | Refresh after the next full Windows run |
