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
