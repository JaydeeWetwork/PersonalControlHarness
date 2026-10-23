"""Disc Test, second seat: the handshake pattern on a real (toy-scale)
DeepSeek-V3 model, through the specimen's own MLA + MoE code.

Checks:
  1. Sampled run (temperature 0.8) replays token-identically.
  2. Greedy run (temperature 0) replays token-identically.
  3. Naive tamper (edit a token, leave seals stale) fails verification.
  4. Sophisticated tamper (edit a token, honestly re-seal the chain) STILL
     fails: re-derivation from the per-step draws catches what checksums
     alone would wave through.
  5. Non-perturbation: running under the harness changes nothing -- a plain
     un-harnessed generation with identical draws produces identical tokens.
"""
import atexit
import copy
import os
import shutil
import sys
import tempfile

import torch

import harness
import stepstream

PROMPT = [11, 42, 7, 99, 3, 250, 18, 5]
RUN_SEED = 777
MAX_NEW = 16

fails = 0

# Fresh streams go to an output dir, never over the committed reference
# streams in pch/harness/ (core fix approved by JD 2026-10-23).
# Set PCH_OUT_DIR to choose it (e.g. to record new reference streams).
OUT_DIR = os.environ.get("PCH_OUT_DIR")
if not OUT_DIR:
    # Throwaway dir: remove it on exit so repeated runs leave nothing behind.
    # A caller-chosen PCH_OUT_DIR is always kept.
    OUT_DIR = tempfile.mkdtemp(prefix="pch-disc-")
    atexit.register(shutil.rmtree, OUT_DIR, ignore_errors=True)
os.makedirs(OUT_DIR, exist_ok=True)
SAMPLED = os.path.join(OUT_DIR, "stream_sampled.jsonl")
GREEDY = os.path.join(OUT_DIR, "stream_greedy.jsonl")


def check(name, ok, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
    if not ok:
        fails += 1


print("== building toy DeepSeek-V3 (specimen code, CPU, float path pinned)")
model = harness.build_model()
print(f"   weights digest: {harness.weights_digest(model)[:16]}...")

print("== 1. sampled run (temp 0.8, per-step seeded) + replay")
records, produced = harness.run_live(model, PROMPT, MAX_NEW, 0.8, RUN_SEED, control_at=6)
stepstream.save_stream(records, SAMPLED)
print(f"   produced tokens: {produced}  (stream: {SAMPLED})")
fresh = harness.build_model()
ok, why = harness.verify_replay(fresh, stepstream.load_stream(SAMPLED))
check("sampled replay verifies", ok, why)

print("== 2. greedy run (temp 0) + replay")
records_g, produced_g = harness.run_live(model, PROMPT, MAX_NEW, 0.0, RUN_SEED)
stepstream.save_stream(records_g, GREEDY)
print(f"   produced tokens: {produced_g}")
fresh = harness.build_model()
ok, why = harness.verify_replay(fresh, stepstream.load_stream(GREEDY))
check("greedy replay verifies", ok, why)

print("== 3. naive tamper (one token edited, seals left stale)")
tampered = copy.deepcopy(records)
for r in tampered:
    if r["kind"] == "step" and r["step"] == 5:
        r["token"] = (r["token"] + 1) % 512
fresh = harness.build_model()
ok, why = harness.verify_replay(fresh, tampered)
check("naive tamper is REFUSED", not ok, why)

print("== 4. sophisticated tamper (token edited, chain honestly re-sealed)")
forged = copy.deepcopy(records)
body = [r for r in forged if r["kind"] != "trailer"]
for r in body:
    r.pop("chain", None)
    if r["kind"] == "step" and r["step"] == 5:
        r["token"] = (r["token"] + 1) % 512
stepstream.seal(body)
forged = body + [{"kind": "trailer", "steps": MAX_NEW, "final_chain": body[-1]["chain"]}]
fresh = harness.build_model()
ok, why = harness.verify_replay(fresh, forged)
check("re-sealed forgery is REFUSED", not ok, why)

print("== 5. non-perturbation (harness observes; it does not touch)")
plain = harness.build_model()
toks = torch.tensor([PROMPT], dtype=torch.long)
with torch.no_grad():
    logits = plain.forward(toks, 0)
seq = [harness.sample_seeded(logits, 0.8, RUN_SEED, 0)]
for t in range(1, MAX_NEW):
    with torch.no_grad():
        logits = plain.forward(torch.tensor([[seq[-1]]]), len(PROMPT) + t - 1)
    seq.append(harness.sample_seeded(logits, 0.8, RUN_SEED, t))
check("un-harnessed run with same draws == harnessed tokens", seq == produced,
      f"{seq} vs {produced}")

print(f"\nDISC TEST (LLM SEAT): {'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
