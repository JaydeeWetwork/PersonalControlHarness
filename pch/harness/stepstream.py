"""Step-stream format for the LithoLattice-style model harness prototype.

A run is a list of JSON records (one per line on disk). Every record is
hash-chained: chain = sha256(prev_chain || canonical_json(record)) so any
edit anywhere invalidates every link after it. The discrete committed
state (token IDs, step records) is what gets hashed -- never float logits.

Record kinds:
  header   -- run identity: weights digest, config digest, run seed, temperature,
              recording platform (os, arch, torch, python; since 2026-10-23)
  prefill  -- the macro-step: prompt chunk geometry + post-prefill cache digest
  control  -- a control event admitted at a step boundary (Pillar 2)
  step     -- one committed decode token (Pillar 1): step index + token ID
  event    -- an outbound fact stamped with its step (Pillar 3), e.g. eos
  trailer  -- step count + final chain hash (the disc's seal). Schema 2
              (since 0.0.0.4) is itself a chained record, so editing any
              trailer field breaks the chain. Legacy trailers (no "schema")
              are read but labeled "legacy-unchained" and cannot be signed.
"""
import hashlib
import json

GENESIS = "0" * 64


def canonical(rec: dict) -> bytes:
    body = {k: v for k, v in rec.items() if k != "chain"}
    # allow_nan=False: NaN/Infinity are not JSON and loads_strict rejects
    # them, so they must never be hashed or signed (review R1).
    return json.dumps(body, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


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


TRAILER_SCHEMA = 2
TRAILER_FIELDS_V2 = {"kind", "schema", "steps", "final_chain", "chain"}
TRAILER_FIELDS_LEGACY = {"kind", "steps", "final_chain"}


def close(records: list) -> list:
    """Seal the body and append a schema-2 trailer that is itself chained."""
    seal(records)
    n = sum(1 for r in records if r.get("kind") == "step")
    trailer = {"kind": "trailer", "schema": TRAILER_SCHEMA, "steps": n,
               "final_chain": records[-1]["chain"]}
    trailer["chain"] = chain_next(records[-1]["chain"], trailer)
    records.append(trailer)
    return records


def check_sealed(records: list):
    """PCH core receipt check. Returns (ok, reason, label) where label is
    "chained" (schema-2 trailer in the hash chain) or "legacy-unchained"."""
    try:
        if len(records) < 3 or not all(isinstance(r, dict) for r in records):
            return False, "structure: too few records or non-object record", None
        if records[0].get("kind") != "header":
            return False, "structure: first record is not a header", None
        trailer = records[-1]
        if trailer.get("kind") != "trailer":
            return False, "structure: missing trailer", None
        body = records[:-1]
        if any(r.get("kind") in ("header", "trailer") for r in body[1:]):
            return False, "structure: header/trailer out of place", None
        if not check_chain(body):
            return False, "chain: hash chain broken", None
        if "schema" in trailer:
            if type(trailer.get("schema")) is not int or trailer["schema"] != TRAILER_SCHEMA:
                return False, "seal: unknown trailer schema", None
            if set(trailer) != TRAILER_FIELDS_V2:
                return False, "seal: trailer has unexpected or missing fields", None
            if trailer["chain"] != chain_next(body[-1]["chain"], trailer):
                return False, "seal: trailer is not in the hash chain (edited trailer)", None
            label = "chained"
        else:
            if set(trailer) != TRAILER_FIELDS_LEGACY:
                return False, "seal: legacy trailer has unexpected or missing fields", None
            label = "legacy-unchained"
        if trailer.get("final_chain") != body[-1].get("chain"):
            return False, "seal: trailer final_chain mismatch", None
        steps = [r.get("step") for r in body if r.get("kind") == "step"]
        if type(trailer.get("steps")) is not int or trailer["steps"] != len(steps):
            return False, "seal: trailer step count mismatch", None
        return True, f"{len(steps)} steps, {label}", label
    except (TypeError, ValueError, AttributeError, KeyError, RecursionError) as e:  # R2
        return False, f"malformed: {e.__class__.__name__}", None


def check_step_sequence(records: list):
    """Step records must be numbered exactly 0..n-1 in order (review R3).
    The chain alone cannot tell, because a forger re-hashes; replay uses this."""
    n = 0
    for r in records:
        if r.get("kind") == "step":
            if type(r.get("step")) is not int or r["step"] != n:
                return False, f"steps: expected step {n}, found {r.get('step')!r}"
            n += 1
    return True, f"{n} steps numbered 0..{n - 1}"


class StrictJSONError(ValueError):
    pass


def _no_dupes(pairs):
    d = {}
    for k, v in pairs:
        if k in d:
            raise StrictJSONError(f"duplicate key {k!r}")
        d[k] = v
    return d


def _no_const(name):
    raise StrictJSONError(f"non-finite number {name}")


def loads_strict(text: str):
    """json.loads that rejects duplicate keys and NaN/Infinity, so one byte
    string has exactly one meaning before anything is hashed or signed."""
    return json.loads(text, object_pairs_hook=_no_dupes, parse_constant=_no_const)


def save_stream(records: list, path: str) -> None:
    # UTF-8 + LF on every OS so a saved stream is byte-identical (review R4).
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for rec in records:
            f.write(json.dumps(rec, sort_keys=True) + "\n")


def load_stream(path: str) -> list:
    with open(path, encoding="utf-8") as f:
        return [loads_strict(line) for line in f if line.strip()]
