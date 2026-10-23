# Blockers and open items (hardening phase 1, 2026-10-23)

Recorded per the hardening brief. Nothing below is faked; each item is either
a documented skip or not started.

| # | Item | Why it is blocked / not done | Needs |
|---|------|------------------------------|-------|
| B1 | Beds 08 HUD reading, 09 Telegraph fairness, 11 HUD-only honesty | Need a HUD / telegraph observation feed and boss attacks with wind-up timing; the PCH reference boss stub does not model a HUD. Skipped with reason (exit 77). Decision: Implement (`testbeds/DECISIONS.md`). | Phase 2: HUD model in the stub, or the LithoKraft boss emitting PCH receipts |
| B2 | Beds 10, 12, 13, 14 run on the reference boss stub only | `pch/harness/boss.py` is a minimal stub, not the game's boss. Bed 12 does not model inventory-swap timing. | Port the checks to the LithoKraft boss |
| B3 | Section 3: real specimen signed receipt | Weights not downloaded (brief: do not download yet). Waits for the engine fixes to land. | JD go-ahead to download |
| B4 | Specimen architecture mismatch | The chosen candidate, `deepseek-ai/DeepSeek-R1-Distill-Qwen-1.5B` (MIT license, HF revision `ad9f0ae`, one `model.safetensors`, about 3.5 GB, under the 4 GB limit), is a **Qwen2** model (`model_type: qwen2`). The vendored DeepSeek-V3 inference code (`third_party/DeepSeek-V3`) cannot load it. | **JD decision:** (a) add a small Qwen2 loader (e.g. `transformers`, Apache-2.0) for the specimen, or (b) pick a different official model with the V3 architecture (none of the official V3-architecture models is 4 GB or less) |
| B5 | Section 5: CI | No CI runner is configured; nothing was pushed (brief: no pushes). `pch.docs_check` runs in ctest locally. | Choose a CI host; approve workflow file |
| B6 | Engine demo pin as a ctest | `lithokraft --demo-test` is not registered in ctest, so the repinned hash is checked by hand and its presence by `pch.docs_check`. | Register `lithokraft --demo-test` as a ctest |
| B7 | README results block | The quoted ctest transcript (65 tests) predates 0.0.0.4; the README table vs ctest output is not auto-compared. | Refresh after the next full Windows run |
