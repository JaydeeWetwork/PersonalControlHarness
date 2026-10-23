"""Test-side receipt verifier layered on pch/harness/stepstream.py.

It is NOT part of PCH core. It applies the checks CloudAtlas's design asks for
on top of the hash chain the PCH ships today: size caps, structure, chain,
trailer seal, contiguous step counter, plus a small ledger for replay /
rollback (session binding + forward-only counter). Authenticity (a
deliberate full-chain recompute, case 3) is checked by the Ed25519 signature
layer, pch/harness/receipt_sig.py, not here.
"""
import json

import stepstream

MAX_LINE = 64 * 1024
MAX_RECORDS = 1 << 17


class Reject(Exception):
    pass


def parse(text):
    if len(text) > MAX_LINE * MAX_RECORDS:
        raise Reject("oversize: receipt exceeds size cap")
    recs = []
    for i, line in enumerate(text.splitlines()):
        if not line.strip():
            continue
        if len(line) > MAX_LINE:
            raise Reject(f"oversize: line {i} exceeds {MAX_LINE} bytes")
        try:
            r = stepstream.loads_strict(line)
        except (ValueError, RecursionError) as e:
            raise Reject(f"malformed: line {i}: {e.__class__.__name__}")
        if not isinstance(r, dict):
            raise Reject(f"malformed: line {i} is not an object")
        recs.append(r)
        if len(recs) > MAX_RECORDS:
            raise Reject("oversize: too many records")
    return recs


def verify(records):
    """Size/structure caps above, then the PCH core check
    (stepstream.check_sealed): chain, trailer seal (schema 2 trailers are
    in the chain; legacy ones are labeled), contiguous step counter."""
    if len(records) < 3:
        raise Reject("truncated: too few records")
    if records[-1].get("kind") != "trailer":
        raise Reject("truncated: missing trailer")
    ok, why, _label = stepstream.check_sealed(records)
    if not ok:
        raise Reject(why)
    sealed = records[:-1]
    steps = [r.get("step") for r in sealed if r.get("kind") == "step"]
    if steps != list(range(len(steps))):
        raise Reject("counter: step indices not contiguous from 0")
    return records[-1]["final_chain"], len(steps)


class Ledger:
    """Accepts receipts per session; rejects replays and rollbacks."""

    def __init__(self):
        self.seen = {}      # final_chain -> session
        self.heads = {}     # session -> (steps, final_chain, records)

    def submit(self, session, text):
        records = parse(text)
        head, n = verify(records)
        if head in self.seen:
            raise Reject(f"replay: receipt already accepted for session {self.seen[head]!r}")
        if session in self.heads:
            last_n, last_head, last_recs = self.heads[session]
            if n <= last_n:
                raise Reject("rollback: counter did not move forward")
            if [r.get("chain") for r in records[:len(last_recs) - 1]] != \
               [r.get("chain") for r in last_recs[:-1]]:
                raise Reject("fork: new head does not extend accepted head")
        self.seen[head] = session
        self.heads[session] = (n, head, records)
        return head


def dumps(records):
    return "".join(json.dumps(r, sort_keys=True) + "\n" for r in records)
