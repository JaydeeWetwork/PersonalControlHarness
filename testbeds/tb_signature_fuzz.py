"""Test bed 15: signature fuzz (S1.8). 10,000 seeded mutations of the
signature sidecar and the receipt trailer (values, types, added/removed
fields, raw byte damage, hex flips in the signature, forger re-chains);
the offline verifier (receipt_sig.verify_receipt, what pch-verify runs) must
reject every one that changes anything, and never crash. Modeled on bed 03.
Designed by: CloudAtlas (bed 03 method, extended to signatures). Built by: Cortana.
Usage: tb_signature_fuzz.py [N] [--seed S]"""
import copy
import os
import random
import sys

import pch_testlib
import stepstream
from receipt_check import Reject, dumps, parse

try:
    import receipt_sig as rs
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError as e:
    print(f"  [FAIL] signing support missing: {e}")
    sys.exit(1)

N = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 10000
SEED = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 20261023
HEX = "0123456789abcdef"


def mutate_value(rng, v):
    if isinstance(v, bool) or v is None:
        return rng.choice([not v, 0, "", None])
    if isinstance(v, int):
        return rng.choice([v + rng.choice([-1, 1, 7, 2**32]), -v - 1, str(v), float(v), True])
    if isinstance(v, str):
        if not v:
            return "x"
        j = rng.randrange(len(v))
        k = rng.random()
        if k < 0.6:
            return v[:j] + rng.choice(HEX.replace(v[j], "") if v[j] in HEX else HEX) + v[j + 1:]
        if k < 0.8:
            return v[:j] + v[j + 1:]
        return rng.choice([v.upper(), v + "0", " " + v, int(len(v)), [v]])
    return rng.choice([0, "x", None])


def main():
    rng = random.Random(SEED)
    src = stepstream.load_stream(os.path.join(pch_testlib.HARNESS_DIR, "stream_sampled.jsonl"))
    body = [{k: v for k, v in r.items() if k != "chain"} for r in src if r["kind"] != "trailer"]
    key = Ed25519PrivateKey.from_private_bytes(bytes(rng.randrange(256) for _ in range(32)))
    pub = key.public_key()  # seeded throwaway key, in memory only
    recs, side = rs.close_and_sign(copy.deepcopy(body), key, timestamp=1792800000)
    base_text, base_side_text = dumps(recs), rs.dumps_sidecar(side)
    base_canon = [stepstream.canonical(x) for x in recs]
    base_chain = [x.get("chain") for x in recs]
    c = pch_testlib.Checks()
    ok, why = rs.verify_receipt(parse(base_text), base_side_text, pub)
    c("clean signed receipt accepted", ok, why)
    false_accepts = crashes = noops = 0
    by_op = {}
    for _ in range(N):
        r, sd = copy.deepcopy(recs), dict(side)
        op = rng.randrange(8)
        by_op[op] = by_op.get(op, 0) + 1
        side_text = None
        if op == 0:  # sidecar value
            f = rng.choice(sorted(sd))
            sd[f] = mutate_value(rng, sd[f])
        elif op == 1:  # sidecar add / drop field
            if rng.random() < 0.5:
                sd.pop(rng.choice(sorted(sd)))
            else:
                sd[rng.choice(["x", "sig2", "Sig", "alg "])] = rng.randrange(9)
        elif op == 2:  # sidecar raw byte damage
            t = rs.dumps_sidecar(sd)
            p = rng.randrange(len(t))
            side_text = t[:p] + chr(rng.randrange(32, 127)) + t[p + 1:]
        elif op == 3:  # signature: flip 1-3 hex chars
            s = list(sd["sig"])
            for _k in range(rng.randrange(1, 4)):
                j = rng.randrange(len(s))
                s[j] = rng.choice(HEX.replace(s[j], ""))
            sd["sig"] = "".join(s)
        elif op == 4:  # trailer value
            f = rng.choice(sorted(r[-1]))
            r[-1][f] = mutate_value(rng, r[-1][f])
        elif op == 5:  # trailer add / drop field
            if rng.random() < 0.5:
                r[-1].pop(rng.choice(sorted(r[-1])))
            else:
                r[-1][rng.choice(["note", "sig", "schema2"])] = rng.randrange(9)
        elif op == 6:  # forger: edit trailer then re-chain it, patch sidecar head
            r[-1]["steps"] = mutate_value(rng, r[-1]["steps"])
            r[-1]["chain"] = stepstream.chain_next(r[-2]["chain"], r[-1])
            sd["chain_head"] = r[-1]["chain"]
        else:  # forger: edit a body record, re-close, patch head (+ maybe timestamp)
            b = [{k: v for k, v in x.items() if k != "chain"} for x in r[:-1]]
            k = rng.randrange(1, len(b))
            fld = rng.choice(sorted(b[k]))
            b[k][fld] = mutate_value(rng, b[k][fld])
            r = stepstream.close(b)
            sd["chain_head"] = r[-1]["chain"]
            if rng.random() < 0.5:
                sd["timestamp"] = sd["timestamp"] + 1
        text = dumps(r)
        if side_text is None:
            side_text = rs.dumps_sidecar(sd) if rng.random() < 0.9 else str(sd)
        try:
            parsed = parse(text)
            same = ([stepstream.canonical(x) for x in parsed] == base_canon
                    and [x.get("chain") for x in parsed] == base_chain)
            try:
                same = same and rs.parse_sidecar(side_text) == side
            except rs.SigError:
                same = False
            acc, _why = rs.verify_receipt(parsed, side_text, pub)
        except Reject:
            acc, same = False, False
        except Exception as e:  # any crash is a failure
            crashes += 1
            print(f"  CRASH op {op}: {e.__class__.__name__}: {e}")
            continue
        if acc and same:
            noops += 1
        elif acc:
            false_accepts += 1
            print(f"  FALSE ACCEPT op {op}")
    print(f"  fuzz: {N} mutations, seed {SEED}, per-op {dict(sorted(by_op.items()))}, "
          f"no-op mutations {noops}")
    c("signature fuzz: zero false accepts", false_accepts == 0, f"{false_accepts} false accepts")
    c("signature fuzz: zero crashes", crashes == 0, f"{crashes} crashes")
    c.finish("SIGNATURE FUZZ (bed 15)")


main()
