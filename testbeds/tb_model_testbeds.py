"""Model-backed PCH test beds (need torch + the DeepSeek-V3 specimen; else 77).

  replay_determinism  Cortana TB-1 / Herominde: same seeded run 16x; receipt
                      hash every 64 steps (and final) must match.
  thread_determinism  Herominde #1: same run at 1, 2, 4, N torch threads;
                      final receipt hash must match.
  rewind_fidelity     Cortana TB-3: seal at a boundary, run N steps, rewind
                      (fresh model + prefix replay), re-run; cache digest at
                      the rewind point and the re-run tokens must match, plus
                      verify_replay on each. N limited by the toy model's
                      max_seq_len (256): N in {1, 64, 240} run (240 = the
                      ceiling: 256 - 8 prompt - 8 sealed); 1024/16384/65536
                      need a longer-context fixture -> reported NOT RUN.
  fork_budget         Cortana TB-2: fork a sealed prefix into 1..32
                      branches over 32 steps, plus 4 and 16 branches over
                      the full 240-step ceiling; record wall time, peak
                      RSS and best-branch score (mean log-prob). Informational:
                      passes if every branch keeps the sealed prefix.
"""
import os
import sys
import time

import pch_testlib

pch_testlib.model_prereqs_or_skip()
import torch  # noqa: E402

import harness  # noqa: E402

PROMPT = [11, 42, 7, 99, 3, 250, 18, 5]
SEED = 777
TEMP = 0.8
c = pch_testlib.Checks()


def run(max_new, threads=1, seed=SEED):
    m = harness.build_model()          # pins 1 thread
    torch.set_num_threads(threads)      # then override for the thread test
    recs, toks = harness.run_live(m, PROMPT, max_new, TEMP, seed)
    return recs, toks


def chain_at(recs, every):
    steps = [r for r in recs if r["kind"] == "step"]
    return [r["chain"] for r in steps if r["step"] % every == every - 1] + [recs[-1]["final_chain"]]


def replay_determinism():
    ref, _ = run(128)
    ref_h = chain_at(ref, 64)
    for i in range(1, 16):
        recs, _ = run(128)
        h = chain_at(recs, 64)
        first = next((k for k, (x, y) in enumerate(zip(ref_h, h)) if x != y), None)
        c(f"run {i + 1}/16 matches run 1", first is None,
          "" if first is None else f"first divergence at checkpoint {first} (x64 steps)")


def thread_determinism():
    n = os.cpu_count() or 1
    counts = sorted({1, 2, 4, n})
    ref, _ = run(64, 1)
    for t in counts[1:]:
        recs, _ = run(64, t)
        c(f"{t} threads == 1 thread", recs[-1]["final_chain"] == ref[-1]["final_chain"])


def rewind_fidelity():
    cap = harness.toy_args().max_seq_len - len(PROMPT)
    n_max = cap - 8          # largest N the toy fixture allows (8 sealed + N <= 248)
    print(f"  ceiling: toy fixture max_seq_len {harness.toy_args().max_seq_len}, prompt {len(PROMPT)}, "
          f"8 sealed steps -> largest N = {n_max}")
    for n in [1, 64, n_max, 1024, 16384, 65536]:
        if 8 + n > cap:
            print(f"  [NOT RUN] N={n}: exceeds toy model context ({cap} steps); "
                  "needs a longer-context fixture")
            continue
        recs, toks = run(8 + n)
        fresh = harness.build_model()
        with torch.no_grad():
            lg = fresh.forward(torch.tensor([PROMPT]), 0)
        snap_ok = harness.cache_digest(fresh, len(PROMPT)) == \
            next(r for r in recs if r["kind"] == "prefill")["cache"]
        prev = harness.sample_seeded(lg, TEMP, SEED, 0)
        out = [prev]
        for t in range(1, 8 + n):
            with torch.no_grad():
                lg = fresh.forward(torch.tensor([[prev]]), len(PROMPT) + t - 1)
            prev = harness.sample_seeded(lg, TEMP, SEED, t)
            out.append(prev)
        ok, why = harness.verify_replay(harness.build_model(), recs)
        c(f"N={n} rewound snapshot digest matches", snap_ok)
        c(f"N={n} re-run after rewind identical", out == toks)
        c(f"N={n} replay determinism (verify_replay)", ok, why)


def fork_budget():
    try:
        import psutil
        rss = lambda: psutil.Process().memory_info().rss  # noqa: E731
    except ImportError:
        rss = lambda: 0  # noqa: E731
    sealed = 8
    h_max = harness.toy_args().max_seq_len - len(PROMPT) - sealed   # 240: fixture ceiling
    base, base_toks = run(sealed)
    print(f"  ceiling: horizon up to {h_max} steps (toy context), branches up to 32")
    for k, horizon in [(1, 32), (2, 32), (4, 32), (8, 32), (16, 32), (32, 32), (4, h_max), (16, h_max)]:
        t0, peak, best, prefix_ok = time.perf_counter(), 0, None, True
        for b in range(k):
            m = harness.build_model()
            with torch.no_grad():
                lg = m.forward(torch.tensor([PROMPT]), 0)
            prev, toks, lp = None, [], 0.0
            for t in range(sealed + horizon):
                if t > 0:
                    with torch.no_grad():
                        lg = m.forward(torch.tensor([[prev]]), len(PROMPT) + t - 1)
                seed = SEED if t < sealed else SEED + 1 + b
                prev = harness.sample_seeded(lg, TEMP, seed, t)
                if t >= sealed:
                    lp += float(torch.log_softmax(lg.float() / TEMP, -1)[0, prev])
                toks.append(prev)
            prefix_ok &= toks[:sealed] == base_toks
            best = lp if best is None else max(best, lp)
            peak = max(peak, rss())
        dt = time.perf_counter() - t0
        print(f"  branches={k:2d} horizon={horizon:3d} wall={dt:7.2f}s peakRSS={peak / 2**20:7.1f}MiB "
              f"best_score={best / horizon:.4f}")
        c(f"{k} branches x {horizon} steps keep sealed prefix", prefix_ok)


{"replay_determinism": replay_determinism, "thread_determinism": thread_determinism,
 "rewind_fidelity": rewind_fidelity, "fork_budget": fork_budget}[sys.argv[1]]()
c.finish(sys.argv[1].upper())
