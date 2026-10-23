"""Step-stream format for the LithoLattice-style model harness prototype.

A run is a list of JSON records (one per line on disk). Every record is
hash-chained: chain = sha256(prev_chain || canonical_json(record)) so any
edit anywhere invalidates every link after it. The discrete committed
state (token IDs, step records) is what gets hashed -- never float logits.

Record kinds:
  header   -- run identity: weights digest, config digest, run seed, temperature
  prefill  -- the macro-step: prompt chunk geometry + post-prefill cache digest
  control  -- a control event admitted at a step boundary (Pillar 2)
  step     -- one committed decode token (Pillar 1): step index + token ID
  event    -- an outbound fact stamped with its step (Pillar 3), e.g. eos
  trailer  -- step count + final chain hash (the disc's seal)
"""
import hashlib
import json

GENESIS = "0" * 64


def canonical(rec: dict) -> bytes:
    body = {k: v for k, v in rec.items() if k != "chain"}
    return json.dumps(body, sort_keys=True, separators=(",", ":")).encode()


def chain_next(prev_chain: str, rec: dict) -> str:
    return hashlib.sha256(bytes.fromhex(prev_chain) + canonical(rec)).hexdigest()


def seal(records: list) -> list:
    """Attach chain hashes in order; returns the records."""
    prev = GENESIS
    for rec in records:
        prev = chain_next(prev, rec)
        rec["chain"] = prev
    return records


def check_chain(records: list) -> bool:
    prev = GENESIS
    for rec in records:
        if rec.get("chain") != chain_next(prev, rec):
            return False
        prev = rec["chain"]
    return True


def save_stream(records: list, path: str) -> None:
    with open(path, "w") as f:
        for rec in records:
            f.write(json.dumps(rec, sort_keys=True) + "\n")


def load_stream(path: str) -> list:
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]
