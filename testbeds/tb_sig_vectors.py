"""S1.2: golden vectors for the signed receipt payload (ADR-001).
Checks (1) the Ed25519 library against RFC 8032 TEST 1, (2) that the payload
encoder reproduces every checked-in payload byte for byte, (3) that every
golden signature verifies and is reproduced (Ed25519 is deterministic), and
(4) that the encoder refuses malformed inputs. Vectors:
pch/testdata/sig_vectors.json (public RFC 8032 test key, labeled).
Designed by: CloudAtlas (canonicalization risk, plan section 1.5). Built by: Cortana."""
import json
import os

import pch_testlib

try:
    import receipt_sig as rs
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError as e:
    print(f"  [FAIL] signing support missing: {e}")
    raise SystemExit(1)

VEC = os.path.join(pch_testlib.HARNESS_DIR, "..", "testdata", "sig_vectors.json")
c = pch_testlib.Checks()
with open(VEC, encoding="utf-8") as f:
    v = json.load(f)
k = v["test_vector_key"]
c("vector key is labeled as a public test vector", "NOT A REAL KEY" in k["label"])
sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(k["private_seed_hex"]))
pk = sk.public_key()
c("RFC 8032 TEST 1 signature reproduced",
  sk.sign(b"").hex() == v["rfc8032_test1"]["signature_hex"])
c("RFC 8032 TEST 1 matches the RFC text",
  v["rfc8032_test1"]["signature_hex"].startswith("e5564300c360ac72")
  and v["rfc8032_test1"]["signature_hex"].endswith("5141438e7a100b"))
for t in v["vectors"]:
    p = rs.signed_payload(t["chain_head"], t["trailer"], t["schema"], t["brain_id"], t["timestamp"])
    c(f"payload bytes match golden ({t['name']})", p.hex() == t["payload_hex"])
    c(f"signature reproduced ({t['name']})", sk.sign(p).hex() == t["signature_hex"])
    try:
        pk.verify(bytes.fromhex(t["signature_hex"]), p)
        c(f"golden signature verifies ({t['name']})", True)
    except Exception as e:
        c(f"golden signature verifies ({t['name']})", False, repr(e))
t = v["vectors"][1]
c("key order in trailer does not change the payload",
  rs.signed_payload(t["chain_head"], dict(reversed(list(t["trailer"].items()))),
                    t["schema"], t["brain_id"], t["timestamp"]).hex() == t["payload_hex"])
bad = {
    "short chain head": ("ab" * 31, t["schema"], t["timestamp"]),
    "non-hex chain head": ("zz" * 32, t["schema"], t["timestamp"]),
    "negative timestamp": (t["chain_head"], t["schema"], -1),
    "bool timestamp": (t["chain_head"], t["schema"], True),
    "float schema": (t["chain_head"], 2.0, t["timestamp"]),
}
for name, (h, s, ts) in bad.items():
    try:
        rs.signed_payload(h, t["trailer"], s, t["brain_id"], ts)
        c(f"encoder refuses {name}", False, "accepted")
    except rs.SigError as e:
        c(f"encoder refuses {name}", True, str(e))
c.finish("SIGNATURE PAYLOAD GOLDEN VECTORS (S1.2)")
