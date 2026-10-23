"""CloudAtlas TB: receipt tamper and replay check (testbeds/designs/
cloudatlas-receipt-tamper-check.md). Runs against the shipped sealed streams.
Case 3 (re-hash forgery) is SKIPPED: receipts are not signed yet. Hash-only
chains catch accidental/partial tampering, not a deliberate full-chain
recompute. Usage: tb_receipt_tamper.py [--fuzz N]"""
import copy
import hashlib
import os
import random
import sys

import pch_testlib
import stepstream
from receipt_check import Ledger, Reject, dumps, parse, verify

H = pch_testlib.HARNESS_DIR
STREAMS = ["stream_sampled.jsonl", "stream_greedy.jsonl"]


def load(name):
    with open(os.path.join(H, name), "rb") as f:
        raw = f.read()
    print(f"  input {name} sha256={hashlib.sha256(raw).hexdigest()}")
    return raw.decode()


def rejected(text, ledger=None, session="s1"):
    try:
        if ledger is not None:
            ledger.submit(session, text)
        else:
            verify(parse(text))
        return True, "ACCEPTED"
    except Reject as e:
        return False, str(e)
    except Exception as e:  # any crash is a failure of the check
        return None, f"CRASH {e.__class__.__name__}: {e}"


def expect_reject(c, name, text, ledger=None, session="s1"):
    acc, why = rejected(text, ledger, session)
    c(name, acc is False, why)


def reseal_prefix(recs, k):
    """An honest, older chain head: first k sealed records + a fresh trailer."""
    body = [copy.deepcopy(r) for r in recs[:k]]
    for r in body:
        r.pop("chain", None)
    stepstream.seal(body)
    n = sum(1 for r in body if r["kind"] == "step")
    return body + [{"kind": "trailer", "steps": n, "final_chain": body[-1]["chain"]}]


def cases(c):
    texts = {n: load(n) for n in STREAMS}
    recs = {n: parse(t) for n, t in texts.items()}
    a, b = recs[STREAMS[0]], recs[STREAMS[1]]
    ta = texts[STREAMS[0]]

    for n, t in texts.items():
        acc, why = rejected(t)
        c(f"clean receipt passes ({n})", acc is True, why)

    # 1 bit flip in payload (one byte of a token digit)
    # change one hex char inside a "cache" digest: stays valid JSON,
    # so the rejection must come from the hash chain
    i = ta.index('"cache": "') + len('"cache": "') + 5
    ch = ta[i]
    flipped = ta[:i] + ("1" if ch == "0" else "0") + ta[i + 1:]
    expect_reject(c, "case 1 bit flip", flipped)

    # 2 field edit, old hash kept
    r = copy.deepcopy(a)
    st = next(x for x in r if x["kind"] == "step" and x["step"] == 3)
    st["token"] = (st["token"] + 1) % 512
    expect_reject(c, "case 2 field edit (old hash kept)", dumps(r))

    print("  [SKIP] case 3 re-hash forgery -- receipts not signed yet; a hash-only "
          "chain cannot stop a forger who recomputes the whole chain (re-derivation "
          "by model replay is the PCH's current defence; signing turns this on)")

    # 4 replay: same receipt twice / into another session
    L = Ledger()
    acc, why = rejected(ta, L, "s1")
    c("case 4 first submission accepted", acc is True, why)
    expect_reject(c, "case 4 replay same session", ta, L, "s1")
    expect_reject(c, "case 4 replay other session", ta, L, "s2")

    # 5 reorder / drop
    r = copy.deepcopy(a)
    s = [k for k, x in enumerate(r) if x["kind"] == "step"]
    r[s[2]], r[s[3]] = r[s[3]], r[s[2]]
    expect_reject(c, "case 5 reorder", dumps(r))
    r = copy.deepcopy(a)
    del r[s[4]]
    expect_reject(c, "case 5 drop", dumps(r))

    # 6 branch swap: branch B's results under branch A's header/identity
    r = [copy.deepcopy(a[0])] + [copy.deepcopy(x) for x in b[1:]]
    expect_reject(c, "case 6 branch swap (B results on A header)", dumps(r))
    r = copy.deepcopy(a)
    r[-1]["final_chain"] = b[-1]["final_chain"]
    expect_reject(c, "case 6 branch swap (B head on A trailer)", dumps(r))

    # 7 rollback: an older, honestly sealed head after a newer one
    L = Ledger()
    sealed = a[:-1]
    older = dumps(reseal_prefix(sealed, len(sealed) - 4))
    newer = dumps(reseal_prefix(sealed, len(sealed) - 1))
    c("case 7 newer head accepted", rejected(newer, L, "s1")[0] is True)
    expect_reject(c, "case 7 rollback to older head", older, L, "s1")

    # 8 truncation and oversize
    expect_reject(c, "case 8 truncated mid-line", ta[: len(ta) // 2])
    expect_reject(c, "case 8 trailer dropped", dumps(a[:-1]))
    expect_reject(c, "case 8 empty", "")
    expect_reject(c, "case 8 oversize line", ta + '{"kind":"step","pad":"' + "x" * 70000 + '"}\n')
    expect_reject(c, "case 8 non-object line", ta + "[1,2,3]\n")


def fuzz(c, n, seed=20261023):
    rng = random.Random(seed)
    base = parse(load(STREAMS[0]))
    canon = [stepstream.canonical(r) for r in base]
    false_accepts = crashes = noops = 0
    for _ in range(n):
        r = copy.deepcopy(base)
        op = rng.randrange(5)
        k = rng.randrange(len(r))
        if op == 0:  # mutate a value
            key = rng.choice(sorted(r[k]))
            v = r[k][key]
            if isinstance(v, bool) or v is None:
                r[k][key] = not v
            elif isinstance(v, (int, float)):
                r[k][key] = v + rng.choice([-1, 1, 7])
            elif isinstance(v, str):
                j = rng.randrange(max(1, len(v)))
                r[k][key] = v[:j] + rng.choice("0123456789abcdefz") + v[j + 1:]
            elif isinstance(v, list) and v:
                v[rng.randrange(len(v))] = rng.randrange(512)
        elif op == 1:
            r[k].pop(rng.choice(sorted(r[k])))
        elif op == 2:
            r[k]["x"] = rng.randrange(1000)
        elif op == 3 and len(r) > 1:
            j = rng.randrange(len(r))
            r[k], r[j] = r[j], r[k]
        else:
            del r[k]
        text = dumps(r)
        if rng.random() < 0.2:  # raw byte damage on top
            p = rng.randrange(len(text))
            text = text[:p] + chr(rng.randrange(32, 127)) + text[p + 1:]
        try:
            parsed = parse(text)
            same = [stepstream.canonical(x) for x in parsed] == canon and \
                [x.get("chain") for x in parsed] == [x.get("chain") for x in base]
        except Exception:
            same = False
        acc, _ = rejected(text)
        if acc is None:
            crashes += 1
        elif acc and same:
            noops += 1  # mutation did not change the receipt
        elif acc:
            false_accepts += 1
    print(f"  fuzz: {n} mutations, seed {seed}, no-op mutations {noops}")
    c("fuzz: zero false accepts", false_accepts == 0, f"{false_accepts} false accepts")
    c("fuzz: zero crashes", crashes == 0, f"{crashes} crashes")


if __name__ == "__main__":
    c = pch_testlib.Checks()
    if "--fuzz" in sys.argv:
        fuzz(c, int(sys.argv[sys.argv.index("--fuzz") + 1]))
        c.finish("RECEIPT TAMPER FUZZ")
    cases(c)
    print("\n  NOTE: hash-only chains catch accidental/partial tampering but not a "
          "deliberate full-chain recompute (case 3, skipped until signing lands).")
    c.finish("RECEIPT TAMPER (cases 1,2,4-8; case 3 skipped)")
