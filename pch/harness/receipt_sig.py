"""Ed25519 receipt signatures for PCH step-streams (ADR-001).

Signed payload (S1.2), byte-exact, little-endian, lp(x) = u32 length + bytes:
  magic "PCH-RECEIPT-SIG\\0" | u16 payload version (1)
  | lp(chain head, 32 raw bytes) | lp(canonical(trailer))
  | u32 trailer schema | lp(brain ID, UTF-8) | u64 timestamp (Unix s, UTC)
"""
import hashlib
import struct

import stepstream

MAGIC = b"PCH-RECEIPT-SIG\x00"
PAYLOAD_VERSION = 1


class SigError(ValueError):
    pass


def _lp(b: bytes) -> bytes:
    return struct.pack("<I", len(b)) + b


def brain_id_of(header: dict) -> str:
    """Brain ID derived from the (hashed) header; never free text."""
    b = header.get("brain")
    if isinstance(b, dict) and isinstance(b.get("id"), str) and type(b.get("version")) is int:
        return f"{b['id']}@{b['version']}"
    w = header.get("weights")
    if isinstance(w, str) and len(w) >= 16:
        return f"weights:{w[:16]}"
    return "unknown"


def signed_payload(chain_head: str, trailer: dict, schema: int, brain_id: str,
                   timestamp: int) -> bytes:
    if not isinstance(chain_head, str) or len(chain_head) != 64:
        raise SigError("chain head must be 64 hex chars")
    try:
        head = bytes.fromhex(chain_head)
    except ValueError:
        raise SigError("chain head is not hex")
    if type(schema) is not int or not 0 <= schema < 2**32:
        raise SigError("schema must be a u32")
    if type(timestamp) is not int or not 0 <= timestamp < 2**64:
        raise SigError("timestamp must be a u64")
    if not isinstance(brain_id, str):
        raise SigError("brain id must be a string")
    return (MAGIC + struct.pack("<H", PAYLOAD_VERSION) + _lp(head)
            + _lp(stepstream.canonical(trailer)) + struct.pack("<I", schema)
            + _lp(brain_id.encode("utf-8")) + struct.pack("<Q", timestamp))


def key_id(public_raw: bytes) -> str:
    return hashlib.sha256(public_raw).hexdigest()[:16]


# ---- keys (S1.4 / S1.6) -------------------------------------------------
SIDECAR_FORMAT = "pch-sig/1"
SIDECAR_FIELDS = {"format", "alg", "key_id", "brain_id", "timestamp", "chain_head", "sig"}
REVOCATION_FORMAT = "pch-revocations/1"


def _crypto():
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        return serialization, ed25519
    except ImportError:  # pragma: no cover
        raise SigError("the 'cryptography' package is required (pip install -r requirements.txt)")


def public_raw(public_key) -> bytes:
    ser, _ = _crypto()
    return public_key.public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)


def load_private_key(path: str, passphrase: bytes = None):
    ser, ed = _crypto()
    with open(path, "rb") as f:
        k = ser.load_pem_private_key(f.read(), password=passphrase)
    if not isinstance(k, ed.Ed25519PrivateKey):
        raise SigError("not an Ed25519 private key")
    return k


def load_public_key(path: str):
    """PEM SubjectPublicKeyInfo (what pch-keygen writes; OpenSSL-compatible)."""
    ser, ed = _crypto()
    with open(path, "rb") as f:
        data = f.read()
    try:
        k = ser.load_pem_public_key(data)
    except ValueError as e:
        raise SigError(f"unreadable public key: {e}")
    if not isinstance(k, ed.Ed25519PublicKey):
        raise SigError("not an Ed25519 public key")
    return k


# ---- sign at close (S1.4) -----------------------------------------------
def sign_receipt(records: list, private_key, timestamp: int = None) -> dict:
    """Sign a closed schema-2 receipt once; returns the sidecar dict."""
    import time
    ok, why, label = stepstream.check_sealed(records)
    if not ok:
        raise SigError(f"refusing to sign a receipt that fails the core check: {why}")
    if label != "chained":
        raise SigError("refusing to sign a legacy (unchained-trailer) receipt; re-close it")
    trailer = records[-1]
    ts = int(time.time()) if timestamp is None else timestamp
    brain = brain_id_of(records[0])
    payload = signed_payload(trailer["chain"], trailer, trailer["schema"], brain, ts)
    return {"format": SIDECAR_FORMAT, "alg": "ed25519",
            "key_id": key_id(public_raw(private_key.public_key())),
            "brain_id": brain, "timestamp": ts, "chain_head": trailer["chain"],
            "sig": private_key.sign(payload).hex()}


def close_and_sign(records: list, private_key, timestamp: int = None):
    """Close a stream (schema-2 trailer) and sign it in one step."""
    stepstream.close(records)
    return records, sign_receipt(records, private_key, timestamp)


def dumps_sidecar(sidecar: dict) -> str:
    import json
    return json.dumps(sidecar, sort_keys=True, separators=(",", ":")) + "\n"


def save_sidecar(sidecar: dict, path: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps_sidecar(sidecar))


def parse_sidecar(text: str) -> dict:
    try:
        d = stepstream.loads_strict(text)
    except (ValueError, RecursionError) as e:
        raise SigError(f"sidecar: malformed JSON ({e.__class__.__name__})")
    if not isinstance(d, dict) or set(d) != SIDECAR_FIELDS:
        raise SigError("sidecar: unexpected or missing fields")
    if d["format"] != SIDECAR_FORMAT or d["alg"] != "ed25519":
        raise SigError("sidecar: unknown format or algorithm")
    for f in ("key_id", "brain_id", "chain_head", "sig"):
        if not isinstance(d[f], str):
            raise SigError(f"sidecar: {f} must be a string")
    if type(d["timestamp"]) is not int:
        raise SigError("sidecar: timestamp must be an integer")
    if len(d["sig"]) != 128:
        raise SigError("sidecar: signature must be 64 bytes (128 hex)")
    try:
        bytes.fromhex(d["sig"])
    except ValueError:
        raise SigError("sidecar: signature is not hex")
    return d


def parse_revocations(text: str) -> set:
    """{"format": "pch-revocations/1", "revoked": [{"key_id", "revoked_at", "reason"}]}"""
    try:
        d = stepstream.loads_strict(text)
    except ValueError as e:
        raise SigError(f"revocation list: malformed JSON ({e})")
    if not isinstance(d, dict) or d.get("format") != REVOCATION_FORMAT or \
            not isinstance(d.get("revoked"), list):
        raise SigError("revocation list: wrong format")
    out = set()
    for e in d["revoked"]:
        if not isinstance(e, dict) or not isinstance(e.get("key_id"), str):
            raise SigError("revocation list: entry without key_id")
        out.add(e["key_id"])
    return out


def verify_receipt(records: list, sidecar, public_key, revoked=()):
    """Offline check: core receipt check + Ed25519 signature. Returns (ok, reason).
    Never raises on hostile input."""
    try:
        if isinstance(sidecar, str):
            sidecar = parse_sidecar(sidecar)
        elif not isinstance(sidecar, dict):
            return False, "sidecar: not an object"
        else:
            sidecar = parse_sidecar(dumps_sidecar(sidecar))
        ok, why, label = stepstream.check_sealed(records)
        if not ok:
            return False, f"receipt: {why}"
        if label != "chained":
            return False, "receipt: legacy unchained trailer cannot carry a signature"
        trailer = records[-1]
        kid = key_id(public_raw(public_key))
        if sidecar["key_id"] != kid:
            return False, f"key: signed by key {sidecar['key_id']!r}, not the given key {kid!r}"
        if kid in revoked:
            return False, f"key: key {kid} is revoked"
        if sidecar["chain_head"] != trailer["chain"]:
            return False, "binding: sidecar chain head does not match the receipt"
        brain = brain_id_of(records[0])
        if sidecar["brain_id"] != brain:
            return False, f"binding: sidecar brain {sidecar['brain_id']!r} != header brain {brain!r}"
        payload = signed_payload(trailer["chain"], trailer, trailer["schema"], brain,
                                 sidecar["timestamp"])
        try:
            public_key.verify(bytes.fromhex(sidecar["sig"]), payload)
        except Exception:
            return False, "signature: Ed25519 verification failed"
        return True, (f"signature OK: key {kid}, brain {brain}, "
                      f"{trailer['steps']} steps, head {trailer['chain'][:16]}")
    except SigError as e:
        return False, str(e)
    except (TypeError, ValueError, AttributeError, KeyError, RecursionError) as e:
        return False, f"malformed: {e.__class__.__name__}"
