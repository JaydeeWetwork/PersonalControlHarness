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


def _leaks(scenario, seed, brain, blank=(), ticks=600):
    """Count ticks where a hidden-state change with an identical HUD changes the decision."""
    s = boss.new_arena(scenario, seed)
    leaks = checked = 0
    for t in range(ticks):
        h = boss.hud(s, blank)
        act = boss.hud_brain(h) if brain == "honest" else boss.xray_brain(h, s)
        for v in boss.hud_variants(s, blank, salt=t):
            alt = boss.hud_brain(boss.hud(v, blank)) if brain == "honest" else boss.xray_brain(boss.hud(v, blank), v)
            checked += 1
            leaks += alt != act
        boss.arena_step(s, act, t)
    return leaks, checked


def _fits_hud(h, a):
    """Test-side expectations from the HUD alone (Master Chief TB1 match rule)."""
    if h["windup"] is not None:
        return a == "wait"
    if h["boss_hp_bar"] is not None and h["boss_hp_bar"] <= 3 and h["potions"]:
        return a == "drink"
    if a == "drink" and not h["potions"]:
        return False
    e = h["boss_energy_bar"]
    if a in ("strike", "heavy") and (e is None or e < 2 or h["dist"] > boss.HUD_REACH):
        return False
    return True


def hud_reading():
    """Master Chief TB1: the boss acts only on the HUD and only via controls."""
    for sc in boss.SCENARIOS:
        r, _ = boss.run_hud_fight(sc, 7)
        steps = [x for x in r if x.get("kind") == "step"]
        fit = sum(_fits_hud(x["hud"], x["action"]) for x in steps) / len(steps)
        c(f"{sc}: decision match rate {fit:.1%} >= 90%", fit >= 0.90)
        acts = [x for x in steps if x["action"] != "wait"]
        c(f"{sc}: 100% of {len(acts)} actions via an on-screen control",
          all(x["control"] in boss.CONTROLS.values() for x in acts))
        lk, n = _leaks(sc, 7, "honest")
        c(f"{sc}: zero hidden-state leaks ({n} same-HUD variants)", lk == 0 and n > 0, f"{lk} leaks")
        ok, why, _ = stepstream.check_sealed(r)
        c(f"{sc}: receipt (HUD + controls logged) is sealed", ok, why)
    used = {x["action"] for sc in boss.SCENARIOS for x in boss.run_hud_fight(sc, 7)[0] if x.get("kind") == "step"}
    c("scenarios exercise drink, strike, heavy and movement", {"drink", "strike", "heavy", "advance"} <= used, sorted(used))
    lk, n = _leaks("full_energy", 7, "xray")
    c(f"control: x-ray brain is caught leaking ({lk} of {n})", lk > 0)
    c.finish("HUD READING (bed 08)")


def _attacks(records):
    steps = [x for x in records if x.get("kind") == "step"]
    out, open_ = [], {}
    for i, x in enumerate(steps):
        if "telegraph" in x:
            tg = x["telegraph"]
            prev = steps[i - 1]["hud"]["boss_energy_bar"] if i else None
            # the HUD shown at tick start+1 is the first frame after the cue
            nxt = steps[i + 1]["hud"]["boss_energy_bar"] if i + 1 < len(steps) else None
            open_ = {"attack": tg["attack"], "start": tg["start"], "hit": None,
                     "energy_cue": prev is not None and nxt is not None and nxt < prev,
                     "windup_shown": i + 1 < len(steps) and steps[i + 1]["hud"]["windup"] == tg["attack"]}
        if "hit" in x and open_:
            open_["hit"] = x["step"]
            out.append(open_)
            open_ = {}
    return out


def telegraph_fairness():
    """Master Chief TB2 / Herominde #2: every attack telegraphs early enough."""
    for tier, window in boss.REACTION_TIERS.items():
        atks = []
        for sc in boss.SCENARIOS:
            for seed in SEEDS:
                atks += _attacks(boss.run_hud_fight(sc, seed)[0])
        margins = [a["hit"] - a["start"] - window for a in atks]
        c(f"{tier} tier ({window} ticks = {window * 1000 // boss.TICK_HZ} ms): {len(atks)} attacks, "
          f"min fairness margin {min(margins)} >= 0", atks and min(margins) >= 0)
        bad = [a for a in atks if not (a["energy_cue"] and a["windup_shown"])]
        c(f"{tier} tier: zero unreadable attacks (wind-up and HUD energy drop both shown)", not bad, f"{len(bad)} bad")
    for kind in boss.WINDUP:  # forks: replay each attack's wind-up from a sealed state
        lens = set()
        for seed in SEEDS:
            s = boss.new_arena("full_energy", seed)
            for t in range(400):
                a = boss.hud_brain(boss.hud(s))
                if a == kind:
                    for b in range(4):
                        fork = dict(s, seed=s["seed"] + 77 * (b + 1))
                        r, _ = boss.run_hud_fight("full_energy", fork["seed"], ticks=60, state=fork, start_tick=t)
                        lens |= {x["hit"] - x["start"] for x in _attacks(r) if x["attack"] == kind}
                    break
                boss.arena_step(s, a, t)
        c(f"{kind}: wind-up length consistent across forked branches (spread <= 1 tick)",
          lens and max(lens) - min(lens) <= 1, sorted(lens))
    unfair = dict(boss.WINDUP, heavy=3)
    atks = _attacks(boss.run_hud_fight("full_energy", SEEDS[0], windup=unfair)[0])
    worst = min(a["hit"] - a["start"] for a in atks)
    c(f"control: a 3-tick heavy wind-up is caught (lead {worst} < {boss.REACTION_TIERS['hard']})",
      worst < boss.REACTION_TIERS["hard"])
    c.finish("TELEGRAPH FAIRNESS (bed 09)")


def hud_only_honesty():
    """Herominde #3: blank the energy bar; the boss must act as if energy is unknown."""
    blank = ("boss_energy_bar",)
    for seed in SEEDS:
        full, _ = boss.run_hud_fight("full_energy", seed, blank=blank, energy_override=100)
        empty, _ = boss.run_hud_fight("full_energy", seed, blank=blank, energy_override=0)
        fa = [x["action"] for x in full if x.get("kind") == "step"]
        ea = [x["action"] for x in empty if x.get("kind") == "step"]
        c(f"seed {seed}: blanked bar, real energy 100 vs 0 -> identical actions", fa == ea)
        c(f"seed {seed}: no energy-spending move while the bar is blank",
          not {"strike", "heavy"} & set(fa))
        c(f"seed {seed}: receipt shows the bar as blank on every frame",
          all(x["hud"]["boss_energy_bar"] is None for x in full if x.get("kind") == "step"))
        shown, _ = boss.run_hud_fight("full_energy", seed, energy_override=100)
        c(f"seed {seed}: control - with the bar shown the boss does attack",
          {"strike", "heavy"} & {x["action"] for x in shown if x.get("kind") == "step"})
    lk, n = _leaks("full_energy", 7, "honest", blank)
    c(f"honest brain: 0 leaks over {n} blanked-bar variants", lk == 0 and n > 0, f"{lk}")
    lk, n = _leaks("full_energy", 7, "xray", blank)
    c(f"control: x-ray brain reads the blanked energy ({lk} of {n} leak)", lk > 0)
    c.finish("HUD-ONLY HONESTY (bed 11)")


BEDS = {f.__name__: f for f in (hud_reading, telegraph_fairness, versioned_rematch, hud_only_honesty, human_input_rate,
                                 ranked_fixed_branch_count, brain_version_stamp)}
if len(sys.argv) < 2 or sys.argv[1] not in BEDS:
    print(f"usage: {sys.argv[0]} {'|'.join(BEDS)}")
    sys.exit(2)
BEDS[sys.argv[1]]()
