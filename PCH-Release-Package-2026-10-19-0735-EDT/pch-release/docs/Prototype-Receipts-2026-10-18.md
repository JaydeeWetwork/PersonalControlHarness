# DeepSeek Harness Prototype — Receipts

*Built and run by Elfie, Sun 2026-10-18 evening EDT, under JD's direction ("do the per-step seeded sampling and stream step logger, replay checker... do all the tests, verify").*
*Code: `~/workspace/deepseek-harness-study/harness/` (`stepstream.py`, `harness.py`, `disc_test_llm.py`, `kernel.py` shim). Streams: `stream_sampled.jsonl`, `stream_greedy.jsonl`. The DeepSeek specimen was imported, never modified.*

## What ran

The specimen's **own** `Transformer` — real MLA attention and MoE routing code — at toy scale on CPU (3 layers, dim 128, 8 routed experts, vocab 512, bf16, single thread so reduction order is pinned). Weights are a **synthetic fixture**: the specimen leaves its Linear weights as uninitialized `torch.empty`, expecting a checkpoint load; with no checkpoint at this scale, the harness fills them deterministically from a seeded generator, and the stream attests that fixture by digest. The generated tokens are therefore gibberish — deliberately. **What is under test is the harness properties, not the prose.**

The three pillars, implemented per the Plug-In Map:
- **Pillar 1:** `sample_seeded()` mirrors the specimen's `sample()` math with the ambient RNG replaced by a draw derived from (run_seed, step). Prefill = one sealed macro-step (chunk geometry + MLA cache digest recorded); decode = a commit at every token.
- **Pillar 2:** a control record admitted at a step boundary (step 6), serialized and hash-chained like everything else.
- **Pillar 3:** header / prefill / step / event facts all leave on the one chained stream; the trailer seals it.

## Results — DISC TEST (LLM SEAT): ALL PASS

1. **Sampled replay (temp 0.8):** 16 committed tokens [138, 42, 251, 22, 175, 55, 234, 256, 507, 183, 376, 488, 208, 128, 28, 149] — replay from the stream alone re-derived **every token**; chain + seal intact.
2. **Greedy replay (temp 0):** 16 tokens re-derived identically.
3. **Naive tamper** (token edited, seals left stale): **REFUSED** — hash chain broken.
4. **Sophisticated tamper** (token edited, chain honestly re-sealed): **REFUSED** — re-derivation caught it at the edited step (`re-derived 55 != recorded 56`). Checksums alone would have waved this forgery through; the per-step draws are the deeper lock. This is Argon's witnessed-replay principle in its same-hardware form.
5. **Non-perturbation:** an un-harnessed run fed the same draws produced identical tokens — the harness observes; it does not touch.

Weights digest (fixture): `4365dc9610eadcfa…`. Runtime ~35 s for the suite on the 2-core workstation.

## Bugs found en route (the test tested the tester)

1. **Specimen finding:** DeepSeek-V3's reference `model.py` never initializes its Linear weights (uninitialized memory until a checkpoint loads) — two "identical" constructions differ in 44 tensors. Recorded because anyone replicating this study will meet it.
2. **Harness bug, caught by the Disc Test itself:** the cache digest read a tensor view's *whole underlying storage*, letting **uncommitted tail state** (a previous run's cache residue) leak into a seal. Fixed by bounding the read to the committed region. The failure mode is the exact one the architecture exists to prevent — noted with appreciation.

## Addendum — PCH Battery #2 (run later the same evening, JD's invitation)

**Rewind and branch (the Additional Renderer's rehearsal move): ALL PASS.** A 16-step stream was rewound to its sealed step-8 state by replaying the prefix alone — prefix bit-identical, prefill cache digest at the rewind point matching — then branched with a different admitted reality at the boundary: the branches share the sealed prefix [138, 42, 251, 22, 175, 55, 234, 256] and diverge after it, and the original stream still verifies untouched. A model can rehearse a different future from any sealed step without disturbing the record of the real one.

**Witnessed replay (Argon's cross-hardware tier): ALL PASS, after one honest correction.** The first witnessed check compared each token against the local best — a wrong criterion for sampled runs on a flat, untrained fixture (first version: forgery waved through; caught and reported, not smoothed). The corrected check implements Argon's actual design: each step record now carries the probability the *live* run saw for its committed token, and the verifier (its arithmetic perturbed by fixed logit drift, standing in for another GPU) checks the probability *it* sees against the recorded one within eps = 0.003. Genuine stream: all 16 recorded probabilities agree under drift. Forgery (step-5 token swapped): **refused — verifier sees p = 0.0025, the record claims 0.0080.** Battery #1 was re-run against the enriched stream format and remains ALL PASS.

## Honest limits

- Toy scale and synthetic weights: architecture properties proven, nothing about the 671B flagship's behavior claimed.
- Same-machine, CPU, single-thread, bf16: this is Argon's **Bit-Exact** tier. The **Witnessed** (cross-hardware, epsilon) tier is designed, not yet built.
- The kernel shim: the specimen's Triton FP8 kernels can't run on CPU; the harness takes the bf16 path only, and the shim fails loudly if an FP8 path is ever reached. Bridge, not a bypass — the specimen is untouched.
