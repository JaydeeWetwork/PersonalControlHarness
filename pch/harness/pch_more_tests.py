"""PCH battery #2 (JD's invitation, 2026-10-18 evening):

A. REWIND AND BRANCH -- the Additional Renderer's core move.
   Run a stream to step 16. Rewind to the sealed state at step 8 by
   replaying only the prefix (prefill + steps 0..7), admit a NEW control
   event at the step-8 boundary, and run a divergent branch. Checks:
   the prefix replays bit-identically, the two branches' first 8 committed
   tokens agree, the branches diverge after the boundary, and the ORIGINAL
   stream still verifies untouched.

B. WITNESSED REPLAY -- Argon's cross-hardware tier, simulated.
   The verifier's arithmetic is perturbed (tiny deterministic logit noise,
   standing in for another GPU's reduction order). Bit-exact re-derivation
   is no longer demanded; instead each recorded token must remain
   near-best under the verifier's own forward pass:
       prob_v(recorded) >= prob_v(best) - eps.
   Checks: the genuine stream passes witnessed verification under drift;
   a stream whose step-5 token was swapped for an implausible one fails it.
"""
import copy
import hashlib

import torch

import harness
import stepstream

PROMPT = [11, 42, 7, 99, 3, 250, 18, 5]
RUN_SEED = 777
MAX_NEW = 16
fails = 0


def check(name, ok, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
    if not ok:
        fails += 1


print("== A. rewind and branch (the rehearsal move)")
model = harness.build_model()
records, produced = harness.run_live(model, PROMPT, MAX_NEW, 0.8, RUN_SEED)
print(f"   original tokens: {produced}")

# Rewind: replay prefix only, into a fresh model, then branch at step 8.
fresh = harness.build_model()
toks = torch.tensor([PROMPT], dtype=torch.long)
with torch.no_grad():
    logits = fresh.forward(toks, 0)
prefix_ok = harness.cache_digest(fresh, len(PROMPT)) == \
    next(r for r in records if r["kind"] == "prefill")["cache"]
prev = harness.sample_seeded(logits, 0.8, RUN_SEED, 0)
replay_prefix = [prev]
for t in range(1, 8):
    with torch.no_grad():
        logits = fresh.forward(torch.tensor([[prev]]), len(PROMPT) + t - 1)
    prev = harness.sample_seeded(logits, 0.8, RUN_SEED, t)
    replay_prefix.append(prev)
check("rewound prefix replays bit-identically", replay_prefix == produced[:8],
      f"{replay_prefix} vs {produced[:8]}")
check("prefill cache digest at rewind point matches", prefix_ok)

# Branch: new control event at the boundary (a different run seed from
# step 8 on = a different admitted reality), continue to step 16.
branch = list(replay_prefix)
prev = branch[-1]
for t in range(8, MAX_NEW):
    with torch.no_grad():
        logits = fresh.forward(torch.tensor([[prev]]), len(PROMPT) + t - 1)
    prev = harness.sample_seeded(logits, 0.8, RUN_SEED + 1, t)  # branch draw salt
    branch.append(prev)
print(f"   branch tokens:   {branch}")
check("branches share the sealed prefix", branch[:8] == produced[:8])
check("branches diverge after the boundary", branch[8:] != produced[8:])

fresh2 = harness.build_model()
ok, why = harness.verify_replay(fresh2, records)
check("original stream still verifies untouched", ok, why)

print("== B. witnessed replay under simulated float drift")
EPS = 0.003


def witnessed_verify(model, records, eps=EPS, drift=1e-3):
    """Argon's criterion: the verifier cannot demand the same token pop out
    of its own (drifted) arithmetic. Instead, each committed token carries
    the probability the LIVE run saw (recorded in the step record); the
    verifier checks the probability IT sees for that token against the
    recorded one, within eps. Post-hoc edits break the agreement: the
    recorded probability belongs to a token the forger no longer has."""
    header = records[0]
    steps = [r for r in records if r["kind"] == "step"]
    toks = torch.tensor([header["prompt"]], dtype=torch.long)
    gen = torch.Generator().manual_seed(1234)  # the 'other GPU': fixed drift
    with torch.no_grad():
        logits = model.forward(toks, 0)
    for rec in steps:
        t = rec["step"]
        if t > 0:
            with torch.no_grad():
                logits = model.forward(
                    torch.tensor([[steps[t - 1]["token"]]]), len(header["prompt"]) + t - 1)
        noisy = logits.float() + drift * torch.randn(
            logits.shape, generator=gen).to(logits.dtype)
        probs = torch.softmax(noisy.float() / header["temperature"], dim=-1)
        p_seen = probs[0, rec["token"]].item()
        if abs(p_seen - rec["p"]) > eps:
            return False, f"step {t}: verifier sees p={p_seen:.4f}, record claims {rec['p']:.4f}"
    return True, f"all {len(steps)} recorded probabilities agree within eps={eps} under drift"


fresh3 = harness.build_model()
ok, why = witnessed_verify(fresh3, records)
check("genuine stream passes witnessed verification", ok, why)

forged = copy.deepcopy(records)
for r in forged:
    if r["kind"] == "step" and r["step"] == 5:
        r["token"] = 3  # an implausible token at this position (live prob ~0)
fresh4 = harness.build_model()
ok, why = witnessed_verify(fresh4, forged)
check("implausible-token forgery fails witnessed verification", not ok, why)

print(f"\nPCH BATTERY #2: {'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
raise SystemExit(1 if fails else 0)
