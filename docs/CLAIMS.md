# Claims register

One row per public capability claim. Status: **verified** (a test passes),
**planned** (not built; the README says "Planned:"), **retired**. A merge
that changes a claim updates this file and `CHANGELOG.md` (see
`CONTRIBUTING.md`). `tools/check_docs.py` (ctest `pch.docs_check`) checks
that every README "Planned:" item has a planned row here and every test named
below is registered in CMake.

| ID | Claim | Evidence (ctest) | Status |
|----|-------|------------------|--------|
| C01 | Every PCH step is hash-chained; an edited link breaks the chain | pch.shipped_streams_chain, pch.receipt_tamper | verified |
| C02 | Replay with the same model re-derives every step (re-hashed forgeries caught) | pch.verify_shipped_streams, pch.replay_determinism | verified |
| C03 | Rewind to a sealed point and branch; the record keeps what happened | pch.rewind_fidelity, pch.battery2_rewind_witness | verified |
| C04 | Same final hash at 1, 2, 4 and all threads | pch.thread_determinism | verified |
| C05 | The trailer is part of the hash chain (schema 2) | pch.trailer_in_chain | verified |
| C06 | Receipts are Ed25519-signed at close; RFC 8032 vectors pass | pch.sig_golden_vectors, pch.sign_at_close | verified |
| C07 | Offline verifier needs only a public key | pch.verify_cli_offline | verified |
| C08 | Re-hashed forgery accepted by hash-only check is rejected by signature check | pch.receipt_tamper | verified |
| C09 | 10,000-mutation fuzz: zero false accepts, zero crashes (receipts, signatures) | pch.receipt_tamper_fuzz, pch.signature_fuzz | verified |
| C10 | Lookahead fork budget measured for 1-16 branches | pch.fork_budget | verified |
| C11 | Vendored DeepSeek-V3 inference code is unmodified at the pinned commit | pch.vendored_specimen | verified |
| C12 | Receipts carry a brain-version stamp and gap-free counter; rematch replays the same | pch.boss.versioned_rematch | verified (reference boss stub) |
| C13 | Boss actions respect a human input-rate cap | pch.boss.human_input_rate | verified (reference boss stub) |
| C14 | Ranked mode: phone and PC budgets give identical decisions | pch.boss.ranked_fixed_branch_count | verified (reference boss stub) |
| C15 | Cross-version replay is refused or labeled, never silent | pch.boss.brain_version_stamp | verified (reference boss stub) |
| C16 | Engine demo scenario hashes to the pinned cities-on hash across record and replay | demo_record_test (engine preset) | verified |
| C17 | Boss acts only on what's on the HUD (bed 08) | pch.boss.hud_reading | planned |
| C18 | Every boss attack has a readable wind-up (bed 09) | pch.boss.telegraph_fairness | planned |
| C19 | Boss cannot read a blanked HUD value (bed 11) | pch.boss.hud_only_honesty | planned |
| C20 | Signed receipt from a real (non-fixture) DeepSeek specimen | - | planned |
| C21 | CI runs the docs check and tests on every push | - | planned |
| C22 | The PCH shows nothing about large-model quality (toy scale only) | - | verified (scope statement) |
