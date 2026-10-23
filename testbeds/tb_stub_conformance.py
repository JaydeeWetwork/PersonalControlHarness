"""stub_conformance: the HUD / telegraph contract that beds 08, 09 and 11 rely on,
asserted against a pluggable boss implementation.

Today the implementation is the PCH reference boss stub (pch/harness/boss.py),
a documented stand-in. The real LithoKraft boss must meet the same contract.
To check it, point PCH_BOSS_IMPL at a Python module (a file path or an
importable name) that exposes the adapter surface:

  TICK_HZ, CONTROLS, ATTACK_COST, WINDUP, REACTION_TIERS, SCENARIOS,
  new_arena(scenario, seed) -> state, hud(state, blank=()) -> dict,
  hud_brain(hud) -> action, arena_step(state, action, tick) -> events

Contract (C1-C8):
  C1 the HUD is a pure, non-mutating function of state, with exactly the
     documented fields;
  C2 the bars are quantized integers 0..10, and hidden state never reaches the HUD;
  C3 every action the brain emits is "wait" or maps to an on-screen control;
  C4 every hit is preceded by a telegraph, and the wind-up is visible on the
     HUD from its first tick to the hit;
  C5 every wind-up is at least the easy (slowest) reaction tier, so it is
     reactable on every tier;
  C6 an attack spends its energy cost at wind-up start (visible as an
     energy-bar drop), and none starts without enough energy;
  C7 fights are deterministic: the same scenario and seed give the same events;
  C8 the brain is a function of the HUD only: two states with the same HUD
     get the same action.
The negative controls are deliberately broken copies of the stub; each one must
fail the clause it breaks, which shows the checks have teeth.
Directed by JaydeeWetwork. Built by Cortana. The contract comes from the beds it
protects: 08 HUD reading (designed by Master Chief), 09 telegraph fairness
(Master Chief and Herominde), 11 HUD-only honesty (Herominde). Proposed in the
DeepSeek model's external review (2026-10-23)."""
import copy
import importlib
import importlib.util
import os
import sys
import types

import pch_testlib  # noqa: F401  (puts the harness on sys.path)

HUD_FIELDS = {"boss_hp_bar", "boss_energy_bar", "potions", "player_hp_bar", "dist", "windup"}
HIDDEN = {"player_cd", "seed"}


def load_impl():
    spec = os.environ.get("PCH_BOSS_IMPL", "").strip()
    if not spec:
        import boss
        return boss, "reference boss stub (pch/harness/boss.py)"
    if os.path.isfile(spec):
        s = importlib.util.spec_from_file_location("pch_boss_impl", spec)
        m = importlib.util.module_from_spec(s)
        s.loader.exec_module(m)
        return m, spec
    return importlib.import_module(spec), spec


def fights(impl, ticks=900):
    for sc in sorted(impl.SCENARIOS):
        for seed in (0, 1, 7, 42):
            s = impl.new_arena(sc, seed)
            log = []
            for t in range(ticks):
                h = impl.hud(s)
                a = impl.hud_brain(types.MappingProxyType(dict(h)))
                before = copy.deepcopy(s)
                ev = impl.arena_step(s, a, t)
                log.append((t, h, a, ev, before))
            yield sc, seed, log


def conform(impl):
    res = {}

    def fail(k, why):
        res.setdefault(k, why)

    easy = max(impl.REACTION_TIERS.values())
    for k, w in impl.WINDUP.items():
        if w < easy:
            fail("C5", f"{k} wind-up {w} ticks < easy reaction tier {easy}")
    runs = list(fights(impl))
    for sc, seed, log in runs:
        open_w = None
        for t, h, a, ev, before in log:
            st = copy.deepcopy(before)
            if impl.hud(st) != impl.hud(st) or st != before:
                fail("C1", f"{sc}/{seed} t{t}: hud() not pure / mutates state")
            if set(h) != HUD_FIELDS:
                fail("C1", f"{sc}/{seed} t{t}: HUD fields {sorted(h)}")
            for f in ("boss_hp_bar", "boss_energy_bar", "player_hp_bar"):
                if type(h[f]) is not int or not 0 <= h[f] <= 10:
                    fail("C2", f"{sc}/{seed} t{t}: {f}={h[f]!r} not a 0..10 bar")
            if HIDDEN & set(h) or any(isinstance(v, dict) for v in h.values()):
                fail("C2", f"{sc}/{seed} t{t}: hidden state on the HUD")
            if a != "wait" and a not in impl.CONTROLS:
                fail("C3", f"{sc}/{seed} t{t}: action {a!r} has no on-screen control")
            if "telegraph" in ev:
                tg = ev["telegraph"]
                cost = impl.ATTACK_COST[tg["attack"]]
                if before["energy"] < cost:
                    fail("C6", f"{sc}/{seed} t{t}: attack started with energy {before['energy']} < {cost}")
                open_w = (tg, t)
            if open_w and t > open_w[1] and "hit" not in ev and h["windup"] != open_w[0]["attack"]:
                fail("C4", f"{sc}/{seed} t{t}: wind-up not visible on the HUD before the hit")
            if "hit" in ev:
                if not open_w or open_w[0]["attack"] != ev["hit"]:
                    fail("C4", f"{sc}/{seed} t{t}: hit {ev['hit']} with no telegraph")
                elif t - open_w[1] < easy:
                    fail("C5", f"{sc}/{seed} t{t}: hit {t - open_w[1]} ticks after its cue < {easy}")
                open_w = None
        # C6 visible: energy bar drops on the tick after a telegraph (cost >= 1 bar).
        for i, (t, h, a, ev, before) in enumerate(log[:-1]):
            if "telegraph" in ev and log[i + 1][1]["boss_energy_bar"] >= h["boss_energy_bar"]:
                fail("C6", f"{sc}/{seed} t{t}: no visible energy drop after the telegraph")
    again = list(fights(impl))
    if [[(t, a, ev) for t, _, a, ev, _ in lg] for _, _, lg in runs] != \
       [[(t, a, ev) for t, _, a, ev, _ in lg] for _, _, lg in again]:
        fail("C7", "same scenario/seed gave different events")
    seen = {}
    for _, _, log in runs:
        for t, h, a, ev, before in log:
            key = tuple(sorted(h.items(), key=lambda x: x[0]))
            if seen.setdefault(key, a) != a:
                fail("C8", f"same HUD {dict(key)} gave {seen[key]!r} and {a!r}")
    return res, sum(len(lg) for _, _, lg in runs)


def mutant(name):
    import boss
    m = types.ModuleType(f"mutant_{name}")
    m.__dict__.update({k: v for k, v in boss.__dict__.items() if not k.startswith("__")})
    m.WINDUP = dict(boss.WINDUP)
    if name == "short_windup":                       # breaks C5
        m.WINDUP["strike"] = 10
        m.arena_step = lambda s, a, t, w=m.WINDUP: boss.arena_step(s, a, t, w)
    elif name == "leaky_hud":                        # breaks C2
        m.hud = lambda s, blank=(): {**boss.hud(s, blank), "boss_energy_bar": s["energy"] / 10}
    elif name == "hidden_windup":                    # breaks C4
        m.hud = lambda s, blank=(): {**boss.hud(s, blank), "windup": None}
        m.hud_brain = lambda h: "strike" if h["dist"] <= 2 else "advance"
    elif name == "free_attack":                      # breaks C6
        def step(s, a, t, w=boss.WINDUP):
            if a in boss.ATTACK_COST and s["windup"] is None:
                s["energy"] += boss.ATTACK_COST[a]   # refund: attack starts with no visible cost
                if s["energy"] < boss.ATTACK_COST[a] * 2:
                    s["energy"] = boss.ATTACK_COST[a] * 2 - 1
            return boss.arena_step(s, a, t, w)
        m.arena_step = step
    elif name == "xray_brain":                       # breaks C8
        state_ref = {}
        def step(s, a, t, w=boss.WINDUP):
            state_ref["s"] = s
            return boss.arena_step(s, a, t, w)
        m.arena_step = step
        m.hud_brain = lambda h: (boss.xray_brain(h, state_ref["s"]) if "s" in state_ref else "wait")
    return m


def main():
    c = pch_testlib.Checks()
    impl, where = load_impl()
    print(f"  implementation under test: {where}")
    res, n = conform(impl)
    for k in ("C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8"):
        c(f"{k} holds on {n} ticks", k not in res, res.get(k, ""))
    if not os.environ.get("PCH_BOSS_IMPL"):
        for name, clause in (("short_windup", "C5"), ("leaky_hud", "C2"), ("hidden_windup", "C4"),
                             ("free_attack", "C6"), ("xray_brain", "C8")):
            r, _ = conform(mutant(name))
            c(f"negative control '{name}' is caught by {clause}", clause in r, r.get(clause, "not caught"))
    c.finish("STUB CONFORMANCE (HUD / telegraph contract)")


if __name__ == "__main__":
    main()
