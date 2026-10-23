"""Test-side receipt verifier layered on pch/harness/stepstream.py.

It is NOT part of PCH core. It applies the checks CloudAtlas's design asks for
on top of the hash chain the PCH ships today: size caps, structure, chain,
trailer seal, contiguous step counter, plus a small ledger for replay /
rollback (session binding + forward-only counter). Receipts are hash-only
(unsigned), so a deliberate full-chain recompute is out of scope (case 3).
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
            r = json.loads(line)
        except ValueError as e:
            raise Reject(f"malformed: line {i}: {e.__class__.__name__}")
        if not isinstance(r, dict):
            raise Reject(f"malformed: line {i} is not an object")
        recs.append(r)
        if len(recs) > MAX_RECORDS:
            raise Reject("oversize: too many records")
    return recs


def verify(records):
    if len(records) < 3:
        raise Reject("truncated: too few records")
    if records[0].get("kind") != "header":
        raise Reject("structure: first record is not a header")
    trailer = records[-1]
    if trailer.get("kind") != "trailer":
        raise Reject("truncated: missing trailer")
    if set(trailer) != {"kind", "steps", "final_chain"}:
        raise Reject("seal: trailer has unexpected or missing fields (trailer is unhashed)")
    sealed = records[:-1]
    if any(r.get("kind") in ("header", "trailer") for r in sealed[1:]):
        raise Reject("structure: header/trailer out of place")
    try:
        if not stepstream.check_chain(sealed):
            raise Reject("chain: hash chain broken")
    except (TypeError, ValueError, AttributeError):
        raise Reject("chain: malformed chain field")
    if trailer.get("final_chain") != sealed[-1].get("chain"):
        raise Reject("seal: trailer final_chain mismatch")
    steps = [r.get("step") for r in sealed if r.get("kind") == "step"]
    if steps != list(range(len(steps))):
        raise Reject("counter: step indices not contiguous from 0")
    if trailer.get("steps") != len(steps):
        raise Reject("seal: trailer step count mismatch")
    return trailer["final_chain"], len(steps)


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
