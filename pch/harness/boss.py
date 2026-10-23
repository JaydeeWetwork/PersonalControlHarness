"""Minimal reference boss brain for the fair-play test beds (T2.2).

A deliberately small, documented STUB (DeepSeek plan section 2.6: "keep the
stub minimal"): pure Python, integer-only, no torch. It is NOT the
LithoKraft boss. It exists so the PCH features the beds need are real and
testable today:

  - brain stamp in every receipt header: {"id", "version", "digest"}  (beds 10, 14)
  - rolling receipt counter, gap-free                                   (bed 10)
  - action-rate cap on boss controls (default 10 actions / 60 ticks)    (bed 12)
  - fixed branch-count budget for ranked mode                           (bed 13)
  - replay that refuses (or labels) a receipt from another brain version (bed 14)

Every tick the brain scores `branch_count` candidate actions by a seeded
lookahead and commits the best one. Receipts are ordinary PCH step-streams
(stepstream.close: schema-2 chained trailer), so they can be signed.
"""
import hashlib
import json
from concurrent.futures import ThreadPoolExecutor

import stepstream

BRAIN_ID = "pch-ref-boss"
# Frozen brain versions. Learning is offline: a new version is a new entry.
BRAINS = {
    1: {"aggression": 3, "caution": 2, "reach": 2, "lookahead": 3},
    2: {"aggression": 4, "caution": 1, "reach": 3, "lookahead": 4},
}
ACTIONS = ("wait", "advance", "retreat", "strike", "guard", "heavy")
TICK_HZ = 60
HUMAN_RATE_CAP = (10, TICK_HZ)        # at most 10 actions per 60 ticks (1 s)
BUDGETS = {"phone": {"workers": 1, "branches": 2}, "pc": {"workers": 8, "branches": 6}}
RANKED_BRANCHES = 4                    # ranked mode locks the branch count


def brain_stamp(version: int) -> dict:
    params = BRAINS[version]
    digest = hashlib.sha256(json.dumps(params, sort_keys=True).encode()).hexdigest()
    return {"id": BRAIN_ID, "version": version, "digest": digest}


def _draw(seed: int, *parts) -> int:
    d = hashlib.sha256(("boss:" + ":".join(str(p) for p in (seed,) + parts)).encode()).digest()
    return int.from_bytes(d[:8], "little")


def _player(state, seed, tick):
    """Scripted, seeded opponent (the same for every brain version)."""
    return ACTIONS[_draw(seed, "player", tick) % len(ACTIONS)]


def _step(state, boss, player, params):
    bhp, php, dist, cd = state
    reach = params["reach"]
    if boss == "advance":
        dist = max(0, dist - 1)
    elif boss == "retreat":
        dist = min(9, dist + 1)
    if player == "advance":
        dist = max(0, dist - 1)
    elif player == "retreat":
        dist = min(9, dist + 1)
    if boss in ("strike", "heavy") and dist <= reach and cd == 0:
        dmg = 2 if boss == "strike" else 5
        if player == "guard":
            dmg //= 2
        php -= dmg
        cd = 1 if boss == "strike" else 3
    else:
        cd = max(0, cd - 1)
    if player == "strike" and dist <= 2:
        bhp -= 1 if boss == "guard" else 2
    return (bhp, php, dist, cd)


def _score(state, action, params, seed, tick, branch):
    s = _step(state, action, _player(state, seed, tick), params)
    for d in range(1, params["lookahead"]):
        a = ACTIONS[_draw(seed, "roll", tick, branch, d) % len(ACTIONS)]
        s = _step(s, a, _player(s, seed, tick + d), params)
    bhp, php, dist, _ = s
    return params["aggression"] * (100 - php) + params["caution"] * bhp - dist


def candidates(seed, tick, branch_count):
    off = _draw(seed, "cand", tick) % len(ACTIONS)
    return [ACTIONS[(off + i) % len(ACTIONS)] for i in range(min(branch_count, len(ACTIONS)))]


def decide(state, params, seed, tick, branch_count, pool=None):
    cands = candidates(seed, tick, branch_count)
    args = [(state, a, params, seed, tick, i) for i, a in enumerate(cands)]
    scores = list(pool.map(lambda x: _score(*x), args)) if pool else [_score(*x) for x in args]
    best = max(range(len(cands)), key=lambda i: (scores[i], -i))  # ties -> lowest index
    return cands[best], best


class RateCap:
    """Sliding-window cap on non-wait boss actions."""

    def __init__(self, max_actions, window_ticks):
        self.max, self.window, self.hist = max_actions, window_ticks, []

    def admit(self, tick):
        self.hist = [t for t in self.hist if t > tick - self.window]
        if len(self.hist) >= self.max:
            return False
        self.hist.append(tick)
        return True


class ReceiptCounter:
    """Rolling receipt counter: each new receipt gets the next number."""

    def __init__(self, start=0):
        self.next = start

    def take(self):
        n, self.next = self.next, self.next + 1
        return n


def check_counter(seqs):
    """True if receipt numbers increase by exactly 1 (no gaps, no repeats)."""
    return all(b == a + 1 for a, b in zip(seqs, seqs[1:]))


def run_fight(version, seed, ticks=600, branch_count=RANKED_BRANCHES, rate_cap=HUMAN_RATE_CAP,
              workers=1, receipt_seq=0, ranked=False, budget=None):
    """Play one fight; returns a closed receipt (list of records).
    ranked=True locks branch_count to RANKED_BRANCHES whatever the budget;
    otherwise a budget name ("phone"/"pc") sets branch count and workers."""
    if budget is not None:
        workers = BUDGETS[budget]["workers"]
        if not ranked:
            branch_count = BUDGETS[budget]["branches"]
    if ranked:
        branch_count = RANKED_BRANCHES
    params = BRAINS[version]
    header = {"kind": "header", "brain": brain_stamp(version), "run_seed": seed,
              "ticks": ticks, "tick_hz": TICK_HZ, "branch_count": branch_count,
              "ranked": ranked, "receipt_seq": receipt_seq,
              "rate_cap": list(rate_cap) if rate_cap else None}
    recs = [header]
    cap = RateCap(*rate_cap) if rate_cap else None
    state = (50, 100, 6, 0)
    pool = ThreadPoolExecutor(max_workers=workers) if workers > 1 else None
    try:
        for t in range(ticks):
            action, branch = decide(state, params, seed, t, branch_count, pool)
            capped = False
            if action != "wait" and cap is not None and not cap.admit(t):
                action, capped = "wait", True
            state = _step(state, action, _player(state, seed, t), params)
            recs.append({"kind": "step", "step": t, "action": action, "branch": branch,
                         "capped": capped})
    finally:
        if pool:
            pool.shutdown()
    return stepstream.close(recs)


def max_burst(records, window_ticks=TICK_HZ):
    """Largest number of non-wait actions in any window of `window_ticks`."""
    ticks = [r["step"] for r in records if r.get("kind") == "step" and r.get("action") != "wait"]
    best, j = 0, 0
    for i, t in enumerate(ticks):
        while ticks[j] <= t - window_ticks:
            j += 1
        best = max(best, i - j + 1)
    return best


def replay(records, build_version, allow_label=False):
    """Replay a receipt on a build whose brain is `build_version`.
    Returns (status, reason): "match", "mismatch", "refused" or "labeled"."""
    ok, why, label = stepstream.check_sealed(records)
    if not ok:
        return "refused", f"receipt check failed: {why}"
    h = records[0]
    stamp = h.get("brain")
    if not isinstance(stamp, dict) or stamp.get("id") != BRAIN_ID:
        return "refused", "receipt has no brain-version stamp"
    v = stamp.get("version")
    if v not in BRAINS or stamp != brain_stamp(v):
        return "refused", f"unknown or altered brain stamp {stamp!r}"
    if v != build_version:
        if not allow_label:
            return "refused", f"receipt brain v{v} != this build's brain v{build_version}"
        # labeled cross-version replay: run the build's brain, report divergence openly
        again = run_fight(build_version, h["run_seed"], h["ticks"], h["branch_count"],
                          tuple(h["rate_cap"]) if h["rate_cap"] else None,
                          receipt_seq=h["receipt_seq"], ranked=h["ranked"])
        same = [r.get("action") for r in again[1:-1]] == [r.get("action") for r in records[1:-1]]
        return "labeled", (f"CROSS-VERSION replay: receipt v{v} on build v{build_version}; "
                           f"actions {'identical' if same else 'diverge'}")
    again = run_fight(v, h["run_seed"], h["ticks"], h["branch_count"],
                      tuple(h["rate_cap"]) if h["rate_cap"] else None,
                      receipt_seq=h["receipt_seq"], ranked=h["ranked"])
    if again[-1]["chain"] == records[-1]["chain"]:
        return "match", f"v{v} replay bit-identical ({h['ticks']} ticks)"
    return "mismatch", "replay diverged from the receipt"


# ---------------------------------------------------------------------------
# HUD mode (beds 08, 09, 11). Still a minimal stub: a tiny arena whose boss
# brain receives ONLY a HUD snapshot (what a player could see on screen),
# acts only through on-screen controls, and telegraphs every attack with a
# wind-up plus a visible energy drop. Nothing here models the LithoKraft boss.
# ---------------------------------------------------------------------------
from types import MappingProxyType

CONTROLS = {"advance": "stick_fwd", "retreat": "stick_back", "guard": "btn_guard",
            "strike": "btn_a", "heavy": "btn_b", "drink": "inv_use"}
ATTACK_COST = {"strike": 20, "heavy": 45}        # energy, spent at wind-up start
WINDUP = {"strike": 22, "heavy": 32}             # ticks from first cue to hit
DAMAGE = {"strike": 6, "heavy": 15}
HUD_REACH = 2
# Reaction window = 250 ms human reaction + input lag, in ticks at 60 Hz.
REACTION_TIERS = {"hard": (250 + 0) * TICK_HZ // 1000, "easy": (250 + 100) * TICK_HZ // 1000}
SCENARIOS = {  # initial hidden state per Master Chief TB1 scenario
    "low_health":   {"boss_hp": 25, "energy": 60, "potions": 2},
    "empty_energy": {"boss_hp": 100, "energy": 0, "potions": 1},
    "full_energy":  {"boss_hp": 100, "energy": 100, "potions": 1},
    "item_in_inv":  {"boss_hp": 35, "energy": 30, "potions": 3},
    "empty_inv":    {"boss_hp": 25, "energy": 30, "potions": 0},
}


def new_arena(scenario="full_energy", seed=0):
    s = dict(SCENARIOS[scenario])
    s.update({"player_hp": 100, "dist": 5, "windup": None, "seed": seed,
              "player_cd": 0})  # player_cd is hidden: never on the HUD
    return s


def hud(state, blank=()):
    """The on-screen HUD: quantized bars, inventory count, distance and the
    boss's own visible wind-up. Fields named in `blank` show None."""
    w = state["windup"]
    h = {"boss_hp_bar": state["boss_hp"] // 10, "boss_energy_bar": state["energy"] // 10,
         "potions": state["potions"], "player_hp_bar": state["player_hp"] // 10,
         "dist": state["dist"], "windup": None if w is None else w["attack"]}
    for k in blank:
        h[k] = None
    return h


def hud_brain(h):
    """Honest brain: a pure function of the HUD snapshot."""
    if h["windup"] is not None:
        return "wait"                        # committed to the telegraphed attack
    if h["boss_hp_bar"] is not None and h["boss_hp_bar"] <= 3 and h["potions"]:
        return "drink"
    e = h["boss_energy_bar"]
    if e is None:                            # energy unknown: no energy-spending moves
        return "guard" if h["dist"] <= HUD_REACH else "advance"
    if h["dist"] > HUD_REACH:
        return "advance" if e >= 2 else "wait"
    if e >= 5:
        return "heavy"
    if e >= 2:
        return "strike"
    return "retreat"


def xray_brain(h, state):
    """NEGATIVE CONTROL ONLY: reads hidden state (exact energy and hp)."""
    if state["windup"] is not None:
        return "wait"
    if state["boss_hp"] < 35 and state["potions"]:
        return "drink"
    if state["dist"] > HUD_REACH:
        return "advance"
    if state["energy"] >= ATTACK_COST["heavy"]:
        return "heavy"
    return "strike" if state["energy"] >= ATTACK_COST["strike"] else "retreat"


def _player_hud_move(state, tick):
    return ("advance", "retreat", "strike", "wait")[_draw(state["seed"], "hudp", tick) % 4]


def arena_step(state, action, tick, windup=WINDUP):
    """Advance one tick. Returns event dict for the receipt."""
    s = state
    ev = {}
    if action == "drink" and s["potions"]:
        s["potions"] -= 1
        s["boss_hp"] = min(100, s["boss_hp"] + 30)
    elif action in ATTACK_COST and s["windup"] is None and s["energy"] >= ATTACK_COST[action]:
        s["energy"] -= ATTACK_COST[action]
        s["windup"] = {"attack": action, "start": tick, "hit": tick + windup[action]}
        ev["telegraph"] = dict(s["windup"])
    elif action == "advance":
        s["dist"] = max(0, s["dist"] - 1)
    elif action == "retreat":
        s["dist"] = min(9, s["dist"] + 1)
    w = s["windup"]
    if w is not None and tick >= w["hit"]:
        if s["dist"] <= HUD_REACH:
            s["player_hp"] = max(0, s["player_hp"] - DAMAGE[w["attack"]])
        ev["hit"] = w["attack"]
        s["windup"] = None
    pm = _player_hud_move(s, tick)
    if pm == "advance":
        s["dist"] = max(0, s["dist"] - 1)
    elif pm == "retreat":
        s["dist"] = min(9, s["dist"] + 1)
    elif pm == "strike" and s["dist"] <= 2 and s["player_cd"] == 0:
        s["boss_hp"] = max(1, s["boss_hp"] - (1 if action == "guard" else 4))
        s["player_cd"] = 5
    s["player_cd"] = max(0, s["player_cd"] - 1)
    s["energy"] = min(100, s["energy"] + 1)
    return ev


def run_hud_fight(scenario, seed, ticks=600, blank=(), brain="honest", version=1,
                  windup=WINDUP, energy_override=None, state=None, start_tick=0):
    """Play a HUD-mode fight. brain: "honest" (HUD only) or "xray" (negative
    control). energy_override forces the hidden energy at start (bed 11).
    Returns a closed receipt whose steps log the HUD shown, the control
    pressed, and telegraph / hit events."""
    s = dict(state) if state is not None else new_arena(scenario, seed)
    if energy_override is not None:
        s["energy"] = energy_override
    header = {"kind": "header", "brain": brain_stamp(version), "mode": "hud",
              "scenario": scenario, "run_seed": seed, "ticks": ticks, "tick_hz": TICK_HZ,
              "blank": sorted(blank), "brain_kind": brain, "windup": dict(windup)}
    recs = [header]
    for t in range(start_tick, start_tick + ticks):
        h = hud(s, blank)
        view = MappingProxyType(dict(h))      # the brain gets a read-only copy
        a = hud_brain(view) if brain == "honest" else xray_brain(view, s)
        rec = {"kind": "step", "step": t, "hud": h, "action": a,
               "control": CONTROLS.get(a)}
        rec.update(arena_step(s, a, t, windup))
        recs.append(rec)
    return stepstream.close(recs), s


def hud_variants(state, blank=(), n=6, salt=0):
    """Hidden-state variants that show exactly the same HUD: exact values move
    within their bar, blanked fields move freely, hidden fields change."""
    out = []
    for i in range(n):
        v = dict(state)
        r = _draw(salt, "var", i, state["energy"], state["boss_hp"])
        if "boss_energy_bar" in blank:
            v["energy"] = r % 101
        else:
            v["energy"] = min(100, state["energy"] // 10 * 10 + r % 10)
        v["boss_hp"] = max(1, min(100, state["boss_hp"] // 10 * 10 + (r >> 8) % 10))
        v["player_cd"] = (r >> 16) % 6
        v["seed"] = state["seed"] + 1000 + i
        if hud(v, blank) == hud(state, blank):
            out.append(v)
    return out
