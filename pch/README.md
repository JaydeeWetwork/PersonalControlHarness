# Personal Control Harness (PCH)

**"It allows a model to personally plan and control its own harness verifiably
before it takes actions."** — JaydeeWetwork, inventor

The Personal Control Harness is a step-serialized control harness for
autoregressive models. A run under the PCH produces a **sealed step-stream**:
a hash-chained record in which every committed token is re-derivable from a
per-step seeded draw. Submit the stream to a verifier and one of two things
happens — the run **replays bit-identically** (Bit-Exact tier), or, across
different arithmetic, every recorded token's live probability is checked
against the verifier's own forward pass within an epsilon (Witnessed tier).
A tampered stream fails either way: edit a token and leave the seals stale,
and the hash chain breaks; edit a token and honestly re-seal the chain, and
re-derivation catches what checksums alone would wave through.

This repository is the reference prototype and its receipts. **The claim is
verifiable: clone it, run it, check it yourself.** Get verified today.

## Honest scope — read this first

- **Toy scale, synthetic weights.** The rig drives the DeepSeek-V3
  *reference implementation's own* MLA + MoE code shrunk to CPU-toy size
  (3 layers, dim 128, 8 routed experts, vocab 512, bf16, single thread).
  The specimen leaves its weights uninitialized until a checkpoint loads,
  so the harness fills them deterministically from a seeded generator; the
  stream attests that fixture by digest (`4365dc9610eadcfa…`). The tokens
  are gibberish by construction. **What is proven is the harness's
  properties, not the prose, and nothing about the 671B flagship's
  behavior is claimed.**
- **Tiers:** Bit-Exact (same arithmetic) — built and receipted. Witnessed
  (cross-hardware, epsilon on recorded probabilities) — built in battery
  #2 against simulated float drift, per the design in
  `docs/Pillar-PlugIn-Map.md`.
- **CPU, bf16 path only.** The specimen's Triton FP8 kernels cannot run on
  CPU; `harness/kernel.py` is a shim that lets the specimen import cleanly
  and **fails loudly** if an FP8 path is ever reached. The specimen is
  imported, never modified.
- Verified on Linux x86-64, Python 3.12, torch 2.14.1+cpu. Full test
  suite runtime ≈ 2 minutes on a 2-core machine.

## The three pillars

1. **Time Serialization.** The sampler's draw at step *t* derives from
   (run_seed, *t*) alone — the stream is self-describing and replayable.
   Committed tokens (discrete state) are what gets hashed, never float
   logits. Prefill is one sealed macro-step; decode commits at every token.
2. **Control Coupling.** Control enters **only at step boundaries**,
   serialized in-stream as records on the same hash chain. Nothing
   interrupts a committed step; an interrupted step simply never commits.
3. **Event Coupling.** Outbound facts (prefill sealed, step probabilities,
   the trailer seal) leave as step-stamped records on the same chain.
   Verification is itself an exchange on this channel: a stream goes out,
   a verdict comes back.

The moral architecture this harness travels under — the Pillars — is in
[`PILLARS.md`](PILLARS.md). Design background, including where the pillars
plug into a production harness and the hazard analysis, is in
[`docs/Pillar-PlugIn-Map.md`](docs/Pillar-PlugIn-Map.md).

## Signed receipts (since 0.0.0.4)

Every stream closes with a schema-2 trailer that is itself in the hash chain,
and can be signed once at close with Ed25519 (`harness.run_live_signed`,
sidecar `<stream>.sig`). Anyone can check a receipt offline:

```sh
python tools/pch_keygen.py --name my-receipts     # key pair in ~/.pch/keys
python tools/pch_sign.py stream.jsonl ~/.pch/keys/my-receipts.key
python tools/pch_verify.py stream.jsonl ~/.pch/keys/my-receipts.pub   # exit 0 = valid
```

Design and byte-exact payload: [`docs/ADR-001-signing.md`](docs/ADR-001-signing.md).
Keys, rotation and revocation: [`docs/KEY-MANAGEMENT.md`](docs/KEY-MANAGEMENT.md).
Golden vectors: `testdata/sig_vectors.json`.

## Quickstart

The DeepSeek-V3 reference code is vendored, unmodified, in
`../third_party/DeepSeek-V3/` (pinned commit `9b4e978`, MIT, see its
`UPSTREAM.md`); the harness finds it automatically and only imports it.
`PCH_SPECIMEN_DIR` overrides the location.

```sh
# 2. Fresh environment (CPU-only torch, as receipted):
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt \
    --index-url https://download.pytorch.org/whl/cpu \
    --extra-index-url https://pypi.org/simple

# 3. Verify the shipped sealed streams, exactly as they sit in this repo:
cd harness
python verify_streams.py        # expect: SHIPPED STREAMS: ALL VERIFY

# 4. Run the full batteries:
python disc_test_llm.py         # expect: DISC TEST (LLM SEAT): ALL PASS
python pch_more_tests.py        # expect: PCH BATTERY #2: ALL PASS
```

`disc_test_llm.py` regenerates `stream_sampled.jsonl` / `stream_greedy.jsonl`
deterministically into a temp folder (or `PCH_OUT_DIR`), never over the
shipped ones; on the same platform they match the shipped set (core fix
approved by JD 2026-10-23: streams carry a hashed platform tag and
`reference_streams/` holds one set per platform). A captured run of exactly these steps, from a fresh venv on
the packaged copy, is kept in [`VERIFY-TRANSCRIPT.txt`](VERIFY-TRANSCRIPT.txt).

### What the batteries check

- **disc_test_llm.py** — sampled (temp 0.8) and greedy (temp 0) runs replay
  token-identically; a naive tamper is refused (chain); a re-sealed forgery
  is refused (re-derivation); an un-harnessed run fed the same draws
  produces identical tokens (non-perturbation).
- **pch_more_tests.py** — rewind to a sealed step and branch with a new
  control event admitted at the boundary (prefix bit-identical, branches
  diverge, the original stream still verifies); witnessed replay under
  simulated float drift (genuine stream agrees within eps = 0.003; an
  implausible-token forgery is refused).

## Layout

```
harness/   stepstream.py (chained stream format), harness.py (the rig),
           disc_test_llm.py, pch_more_tests.py, verify_streams.py,
           kernel.py (CPU shim), stream_sampled.jsonl, stream_greedy.jsonl
           (Linux reference set), reference_streams/<platform>/ (others)
docs/      Pillar-PlugIn-Map.md, Disc-Test-Receipts-2026-10-18.md,
           Prototype-Receipts-2026-10-18.md, Argon-Review-2026-10-18.md,
           Archie-Review-Prototype-Receipts-2026-10-18.md
PILLARS.md         the moral architecture (verbatim)
VERIFY-TRANSCRIPT.txt   a stranger's-eye run of the quickstart, captured
```

## Credits

Inventor: **JaydeeWetwork** (Wet Wafer Works Games Incorporated) —
design, direction, and the pillars. Co-implementation by AI collaborators;
independent design review by **Archie** (the reviews are in `docs/`,
including the hazard ranking and the two-tier replay design).

## License

MIT — see [`LICENSE`](LICENSE). *(Working draft, to be confirmed by the
maintainer before publication.)*

## Model weights

The harness's own tests use tiny seeded fixture weights and need no download.
To use real weights you downloaded yourself, the PCH looks in this order:

1. `--weights <path>` on the command line (`pch/tools/pch_weights.py`);
2. the `PCH_WEIGHTS_DIR` environment variable;
3. `"weights_dir"` in a JSON config file: the file named by `PCH_CONFIG`,
   else `./pch.config.json`, else `%APPDATA%\pch\pch.config.json` (Windows) or
   `~/.config/pch/pch.config.json` (Linux). A relative path is relative to the
   config file. Example: `{"weights_dir": "D:/My Models/DeepSeek-R1-Distill-Qwen-1.5B"}`;
4. the default folder `models/weights/` at the repo root (gitignored).

Windows (`C:\Models\My Model`) and POSIX paths, spaces, `~` and environment
variables all work. A folder must contain `config.json` and `*.safetensors`
(or `model.safetensors.index.json` with every shard); otherwise
`pch_weights.py` exits 1 and says what is missing. Tests: `pch.weights_path`
(precedence and bad paths, always runs) and `pch.weights_local` (validates your
configured folder; skipped when none is present).

`PCH_WEIGHTS_DIR` (model **weights**) and `PCH_SPECIMEN_DIR` (specimen
**code**, default `third_party/DeepSeek-V3/inference`) are independent.
