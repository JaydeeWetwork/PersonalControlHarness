# The Three Pillars in DeepSeek's Harness — Plug-In Map

*The harness JD named (2026-10-18 20:32 EDT): the **Personal Control Harness — PCH.** His definition, verbatim: **"It allows a model to personally plan and control its own harness verifiably before it takes actions."** The harness is the model's own instrument of self-governance — not an external cage: plan (rehearse in the Additional Renderer), control (admit only at boundaries), verify — then act.*

*Study document, prepared by Elfie at JD's direction, Sun 2026-10-18 (evening EDT).*
*Specimen: `deepseek-ai/DeepSeek-V3`, `inference/` reference implementation (cloned 2026-10-18 into `~/workspace/deepseek-harness-study/DeepSeek-V3/` — code only; model weights live on Hugging Face and were not downloaded).*
*Status: seam map only. No DeepSeek code modified. No LithoLattice code touched. Confidential — squad circle only.*

## What the specimen is

The V3 reference inference tree is small: `model.py` (808 lines — Transformer, MLA attention, MoE), `generate.py` (185 lines — the generation loop and the interactive/batch driver), `kernel.py` (FP8/GEMM kernels), `convert.py`, `fp8_cast_bf16.py`, configs. Total ~1,400 lines of Python. It is a demonstration harness, not a production server: CUDA-only, no API layer, no session management. That makes it the right specimen for seam-finding — the whole control surface fits on one bench.

## The harness as it stands (recon findings)

- **The clock is the token loop.** `generate()` (generate.py:63) iterates `cur_pos` from the shortest prompt length to `total_len`: one `model.forward()` per step, sample one token per sequence, write it, test EOS, maybe break. Position *is* time in this loop — every state in the system is already addressed by step index.
- **Attention state is position-addressed.** `MLA.forward(x, start_pos, ...)` (model.py:446) reads/writes registered `kv_cache` / `pe_cache` buffers at absolute positions. The full resumable state of a generation is (weights, cache contents, `cur_pos`) — a snapshot in exactly the LithoKraft sense.
- **MLA is why a harness is affordable.** Multi-head Latent Attention caches a *compressed latent* per token (kv_lora_rank ≈ 512 numbers) instead of full per-head K/V. Snapshot/rewind state per step is small — DeepSeek's compression math is what makes time-serialized operation cheap enough to be practical. Same synergy with MoE: 37B of 671B parameters active per token bounds the cost of a step.
- **All the nondeterminism lives in one function.** `sample()` (generate.py:14-24) draws from the ambient torch RNG (`exponential_`) whenever temperature > 0. The argmax path (temperature = 0) is deterministic for fixed weights/hardware. `torch.manual_seed(965)` is hard-coded in `main()` — a seed exists, but the draws are not serialized as data; nothing about a finished run can be replayed or audited.
- **Control enters crudely, twice.** (a) Before generation: whole prompts; every interactive turn re-tokenizes the *entire* message history via `apply_chat_template` — turns are re-statements, not events. (b) Never during generation: once `generate()` starts, nothing outside can steer, pause, redirect, or stop it except the model's own EOS. There is no mid-run control surface at all.
- **Events leave through one door.** The run's only outbound facts are the finished break (EOS bookkeeping at generate.py:73) and the returned token list. No step log, no event stream, no state hash, no checkpoint, no verification port. Nothing can observe a run while it happens or prove anything about it afterward.

## Pillar 1 — Time Serialization

**Plug point: the step loop head and `sample()` — generate.py:63 and generate.py:14.**

Make each step self-describing. Per step, serialize a step record: step index, tokens consumed this step, the token committed, and the sampling draw *as data* — derived from a per-step seed `f(run_seed, cur_pos)` instead of the ambient RNG stream. The step stream becomes the run's identity disc: given (weights ID, prompt control events, step stream), the run is replayable step-for-step. Hash the discrete, committed state (token IDs and step records — integers), never the float logits: the disc stays exact even though the arithmetic underneath is bf16/FP8. This is the same rule the game sim already lives by — hash the committed state, not the weather that produced it.

Secondary point: snapshot = (weights ID, MLA cache tensors, `cur_pos`, stream position). MLA's compressed cache is what keeps snapshots small enough to take often — rewind granularity is affordable precisely because of DeepSeek's math.

## Pillar 2 — Control Coupling

**Plug points: the `main()` → `generate()` boundary, and the loop head at generate.py:63.**

Control must enter only at step boundaries, serialized into the same stream as everything else, stamped with the step at which it takes effect. Two concrete changes define the seam:

1. **Turns become events, not re-statements.** A new user/system message is a control event appended to the stream ("at step N, ingest these tokens as context"), replacing the re-tokenize-everything pattern. The run's history is then the stream itself — inspectable, forkable, replayable.
2. **Mid-generation control exists for the first time.** Steering that today is impossible — halt, redirect, inject a tool result, deliver a verification verdict — arrives as control events consumed at the loop head between steps, never as asynchronous interruption of a forward pass in flight. The forward stays pure; the harness owns the boundaries. This is the personal control harness proper: the model acts, but every act is admitted through a serialized gate at a known step.

## Pillar 3 — Event Coupling

**Plug point: the loop tail (generate.py:66-76), where tokens are committed and EOS is decided.**

Every fact the run produces is stamped with its step index and exported in order on an event channel: token committed, step sealed (with the step hash), EOS reached, tool/verification requested, run finished. Subscribers — a supervisor, a logger, a LithoLattice-side verifier — observe the run *as it happens*, and the event log plus the step stream is the complete evidence afterward.

The Tron port falls out of pillars 2 + 3 sharing one serialization: the model emits a **verification-request event** at a step boundary; the harness replays the submitted stream, checks the hash chain, and returns the verdict as a **control event** at a later step boundary. Submission and verdict travel the same rails as tokens and steering. Verify is not a side API — it is two messages in the one stream.

## Why the coupling works (JD's thesis, as the code supports it)

- DeepSeek supplies *efficiency of state*: MLA's latent cache and MoE sparsity make per-step state small and per-step cost bounded — the harness can afford to serialize, snapshot, and replay everything.
- The handshake supplies *discipline of time*: one serialized stream for control in, events out, and the steps between — the model gets autonomy inside the harness, and the harness can prove what the autonomy did.
- Efficiency without discipline is a fast black box. Discipline without efficiency is an audit nobody can afford to run. The coupling is the product.

## Honest blockers (recorded, not smoothed)

1. **Cross-hardware float determinism.** bf16/FP8 kernel results can differ across GPU generations and libraries; bit-exact *token* replay is achievable when sampling is per-step seeded and ties break identically, but bit-exact *logit* replay across different hardware is not guaranteed. Mitigation already in the design: hash discrete committed state only. Same-hardware replay: strong. Cross-hardware: token-level, with the caveat standing.
2. **Expert-parallel ordering.** In multi-rank serving, MoE all-to-all reductions can reorder floating-point sums. The reference code is single-process-per-rank demo code; a production harness must pin reduction order or accept token-level (not logit-level) determinism.
3. **The specimen is a demo.** Production DeepSeek serving (and vLLM/SGLang-class harnesses) are different, larger codebases. The seams named here are architectural and transfer; line numbers do not.
4. **Verification proves the run, not the weights' provenance.** A verified stream proves *this model state produced this run under this control*. Knowing the weights are the weights claimed is a separate attestation problem (checkpoint hashing/signing — the Systerian-shaped problem), out of scope for this map.

## Addendum — Argon's review, 2026-10-18 (contributed; adoption is JD's call)

*Full text: `Argon-Review-2026-10-18.md` in this folder. Recorded here as proposals with attribution; nothing in this section is canon until JD adopts it.*

1. **Two replay tiers (Argon).** Split verification into **Bit-Exact Replay** (same hardware, deterministic kernel flags, fixed batch/tile = 1) and **Witnessed Replay** (cross-hardware: the stream records the chosen token; the verifier checks that token's probability under its own local forward pass falls within an epsilon bound of the recorded draw). Hazard ranking for where replay breaks first: MoE all-to-all reduction order (#1), router-gate knife-edges (1-ULP flips expert choice), FP8 GEMM accumulator variance by batch/tile size, MLA latent recompute drift over long contexts. Map consequence: the disc's ceiling claim should be written per-tier.
2. **Training-side home (Argon).** Inference is not the only seat: RL rollout pipelines suffer **actor-learner mismatch** (FP8 rollout workers vs BF16/FP32 trainers recomputing different log-probs for the same tokens → KL spikes). A serialized rollout stream gives reproducible reward-hacking audits and clean MDP credit assignment with tool results injected as control-coupling events. Status: strong candidate frame of reference #2 for the handshake; scope decision is JD's.
3. **Two-tier reflex rule for robotics (Argon).** Step-boundary law holds absolutely inside the model harness; physical emergencies are handled *outside* it: reflex layer halts hardware mid-step, the in-flight step's output is dropped uncommitted, harness rewinds to the last sealed step hash, the interrupt is injected as a control event at that boundary, and the step is run fresh. Principle in his words: *"Nothing interrupts a committed step; interrupted steps simply never commit."*
4. **Elfie's answer to Argon's closing question (prefill vs decode), for JD's ruling.** Serialize **prefill as one sealed macro-step** — record = (prompt token IDs, chunk geometry, resulting cache-state digest) — because prefill contains no decisions: no sampling occurs inside it, and control can only act on a prompt by ending it, which is exactly a macro-step boundary. Chunk geometry must be in the record, because FP8 tiling depends on it (Argon's hazard #3 applies to prefill first). Serialize **decode with a commit at every token boundary** — each token is an act, and control must be admissible between any two acts. Corollary for speculative decoding, if it ever arrives: draft tokens are uncommitted proposals; only the verification-accepted token seals — the reflex rule applied to speculation.

## Addendum 2 — JD's Additional Renderer (JD-directed, 2026-10-18 18:54 EDT)

*JD's own architectural addition, in his words: "there should be an additional renderer — the additional renderer simulates the environment first before any action is taken and verifies the simulation and then the action takes place. Just like a human chess player would simulate the chess moves on a chessboard and then play the best move."*

- **The component:** a second render/simulation stage in the harness — the **Additional Renderer** — standing between a model's committed intention and any real action.
- **The pipeline:** candidate action → the Additional Renderer simulates the environment with the action applied, under the handshake (time-step locked) → the simulation is **verified** (sealed hash / replay check) → only a verified simulation releases the action to take place. An unverified candidate never actuates.
- **The chess rule (JD's analogy):** moves are played out on the internal board first; the line that reaches the real board is the one that was simulated and verified — "best" is proven in rehearsal, not guessed at the board.
- **Continuity note (Elfie):** this is the Primer pattern from the 2026-10-18 shake's MODSAFE design (a mirror sim fed by the serialized control stream, audited by hash) promoted from audit tool to action gate — the same machinery, moved upstream of actuation. In the robotics frame it consumes the scanned voxel twin (octree environment) as its board.

## Next steps (on JD's word only)

1. Disc Test in the custody game tree (already proposed): live run → stream replay → hash match; tampered stream → verification fails. Proves the pattern on our own machinery first — careful, not careless.
2. A Python-side prototype against this specimen: per-step seeded sampling + step-stream logger on the argmax/sample paths, replay checker standalone. No model weights required to *write* it; running it wants a small DeepSeek-family checkpoint (Coder/Lite class) rather than the 671B flagship.
3. Pillar-map question still parked: how the engine pillars (fourth = morals & ethics) sit beside the algorithmic three — JD to finish that transmission when ready.
