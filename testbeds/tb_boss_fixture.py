"""Fair-play test beds 10, 12, 13, 14 on the PCH reference boss brain
(pch/harness/boss.py, a documented minimal stub, not the LithoKraft boss).
Usage: tb_boss_fixture.py versioned_rematch|human_input_rate|
       ranked_fixed_branch_count|brain_version_stamp
Designs: testbeds/designs/ (Master Chief TB3; Herominde #4, #5, #6). Built by: Cortana."""
import copy
import os
import sys

import pch_testlib
import boss
import stepstream

SEEDS = [11, 23, 37, 41, 59]
c = pch_testlib.Checks()


def versioned_rematch():
    """Master Chief TB3: same seed + same brain version -> same receipt, at
    every worker count; v1 rematch after v2 exists matches the v1 originals;
    every receipt names its brain; the rolling counter has no gaps."""
    counter = boss.ReceiptCounter()
    originals, seqs = {}, []
    workers = [1, 2, 4, os.cpu_count() or 4]
    for v in (1, 2):
        for s in SEEDS:
            heads = set()
            for w in workers:
                r = boss.run_fight(v, s, workers=w, receipt_seq=counter.take())
                seqs.append(r[0]["receipt_seq"])
                # receipt hash excluding the per-receipt counter: compare decisions
                heads.add(tuple(x["action"] for x in r[1:-1]))
                c(f"v{v} seed {s} workers {w}: stamp names brain v{v}",
                  r[0]["brain"] == boss.brain_stamp(v))
                originals.setdefault((v, s), r)
            c(f"v{v} seed {s}: identical decisions at workers {workers}", len(heads) == 1)
    for s in SEEDS:  # rematch: v1 again, after v2 exists
        orig = originals[(1, s)]
        again = boss.run_fight(1, s, receipt_seq=orig[0]["receipt_seq"])
        c(f"v1 rematch seed {s}: receipt hash matches the original v1 receipt",
          again[-1]["chain"] == orig[-1]["chain"], again[-1]["chain"][:16])
        st, why = boss.replay(orig, build_version=1)
        c(f"v1 receipt seed {s} replays on a v1 build", st == "match", why)
    c("rolling counter: gap-free and no repeats", boss.check_counter(seqs), f"0..{seqs[-1]}")
    c("counter check catches a gap", not boss.check_counter([0, 1, 3]))
    c("counter check catches a repeat", not boss.check_counter([0, 1, 1, 2]))
    differ = sum(originals[(1, s)][-1]["chain"] != originals[(2, s)][-1]["chain"] for s in SEEDS)
    c("v1 and v2 are really different brains (receipts differ)", differ == len(SEEDS), f"{differ}/{len(SEEDS)}")
    c.finish("VERSIONED RECEIPTS AND REMATCH (bed 10)")


def human_input_rate():
    """Herominde #4: no burst of boss actions exceeds a human rate cap
    (10 actions per second at 60 ticks/s)."""
    cap, win = boss.HUMAN_RATE_CAP
    for v in (1, 2):
        for s in SEEDS:
            r = boss.run_fight(v, s)
            b = boss.max_burst(r, win)
            c(f"v{v} seed {s}: max burst {b} <= {cap} per {win} ticks", b <= cap)
            c(f"v{v} seed {s}: cap recorded in the hashed header", r[0]["rate_cap"] == [cap, win])
    # negative control: without the cap the brain does exceed a human rate,
    # so the check above has teeth
    r = boss.run_fight(2, SEEDS[0], rate_cap=None)
    b = boss.max_burst(r, win)
    c(f"control: uncapped brain bursts {b} > {cap} (the cap is doing real work)", b > cap)
    capped = boss.run_fight(2, SEEDS[0])
    n = sum(1 for x in capped if x.get("capped"))
    c("capped actions are recorded in the receipt", n > 0, f"{n} actions held to 'wait'")
    c("note: inventory-swap timing (part of Herominde #4) is not modeled by the stub", True,
      "documented gap, see testbeds/DECISIONS.md")
    c.finish("HUMAN INPUT RATE (bed 12)")


def ranked_fixed_branch_count():
    """Herominde #5: same fight on a "phone" and a "PC" budget with the branch
    count locked -> identical decisions."""
    for v in (1, 2):
        for s in SEEDS:
            phone = boss.run_fight(v, s, ranked=True, budget="phone")
            pc = boss.run_fight(v, s, ranked=True, budget="pc")
            c(f"v{v} seed {s}: ranked phone == ranked PC (receipt hash)",
              phone[-1]["chain"] == pc[-1]["chain"], phone[-1]["chain"][:16])
            c(f"v{v} seed {s}: branch count locked at {boss.RANKED_BRANCHES} and recorded",
              phone[0]["branch_count"] == pc[0]["branch_count"] == boss.RANKED_BRANCHES)
    diff = 0
    for s in SEEDS:
        a = boss.run_fight(2, s, budget="phone")
        b = boss.run_fight(2, s, budget="pc")
        diff += [x["action"] for x in a[1:-1]] != [x["action"] for x in b[1:-1]]
    c("control: unranked budgets do change decisions (why ranked locks the count)",
      diff > 0, f"{diff}/{len(SEEDS)} fights differ")
    c.finish("FIXED BRANCH COUNT, RANKED (bed 13)")


def brain_version_stamp():
    """Herominde #6: every receipt carries the boss version; a v1 receipt on a
    v2 build is refused or labeled, never silently replayed."""
    r1 = boss.run_fight(1, SEEDS[0])
    c("receipt header carries brain id, version and digest",
      set(r1[0]["brain"]) == {"id", "version", "digest"} and r1[0]["brain"]["version"] == 1)
    st, why = boss.replay(r1, build_version=1)
    c("v1 receipt on v1 build: replays bit-identically", st == "match", why)
    st, why = boss.replay(r1, build_version=2)
    c("v1 receipt on v2 build: refused", st == "refused", why)
    st, why = boss.replay(r1, build_version=2, allow_label=True)
    c("v1 receipt on v2 build with labeling: labeled CROSS-VERSION", st == "labeled"
      and "CROSS-VERSION" in why, why)
    t = copy.deepcopy(r1)
    t[0]["brain"]["version"] = 2
    st, why = boss.replay(t, build_version=2)
    c("stamp edited v1 -> v2: refused (chain breaks)", st == "refused", why)
    body = [{k: v for k, v in x.items() if k != "chain"} for x in r1[:-1]]
    body[0]["brain"] = dict(body[0]["brain"], version=2)  # forger re-closes
    st, why = boss.replay(stepstream.close(body), build_version=2)
    c("stamp edited and re-closed (version/digest mismatch): refused", st == "refused", why)
    body = [{k: v for k, v in x.items() if k != "chain"} for x in r1[:-1]]
    body[0].pop("brain")
    st, why = boss.replay(stepstream.close(body), build_version=1)
    c("receipt without a stamp: refused", st == "refused", why)
    try:
        import receipt_sig as rs
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        k = Ed25519PrivateKey.generate()
        side = rs.sign_receipt(r1, k)
        c("signature binds the brain stamp (sidecar brain_id)", side["brain_id"] == "pch-ref-boss@1",
          side["brain_id"])
        ok, why = rs.verify_receipt(r1, dict(side, brain_id="pch-ref-boss@2"), k.public_key())
        c("sidecar claiming another brain version: rejected", not ok, why)
    except ImportError as e:
        c("signature binds the brain stamp", False, f"signing missing: {e}")
    c.finish("BRAIN-VERSION STAMP (bed 14)")


BEDS = {f.__name__: f for f in (versioned_rematch, human_input_rate,
                                 ranked_fixed_branch_count, brain_version_stamp)}
if len(sys.argv) < 2 or sys.argv[1] not in BEDS:
    print(f"usage: {sys.argv[0]} {'|'.join(BEDS)}")
    sys.exit(2)
BEDS[sys.argv[1]]()
