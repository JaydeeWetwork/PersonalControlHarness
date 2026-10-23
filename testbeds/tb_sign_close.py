"""S1.4: sign once at stream close. Builds a schema-2 receipt from the shipped
stream's records (no model needed), signs it at close with a throwaway key
generated in a temp folder, verifies it, and measures close vs close+sign time.
Keys are never written into the repo. Designed by: CloudAtlas. Built by: Cortana."""
import copy
import os
import time

import pch_testlib
import stepstream

try:
    import receipt_sig as rs
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError as e:
    print(f"  [FAIL] signing support missing: {e}")
    raise SystemExit(1)

c = pch_testlib.Checks()
src = stepstream.load_stream(os.path.join(pch_testlib.HARNESS_DIR, "stream_sampled.jsonl"))
body = [{k: v for k, v in r.items() if k != "chain"} for r in src if r["kind"] != "trailer"]
key = Ed25519PrivateKey.generate()  # in memory only

recs, side = rs.close_and_sign(copy.deepcopy(body), key, timestamp=1792800000)
ok, why = rs.verify_receipt(recs, side, key.public_key())
c("signed at close verifies", ok, why)
c("one signature per receipt (sidecar, not per step)",
  not any("sig" in r for r in recs) and set(side) == rs.SIDECAR_FIELDS)
c("sidecar binds the trailer's own chain link", side["chain_head"] == recs[-1]["chain"])
c("brain id derived from header", side["brain_id"] == rs.brain_id_of(recs[0]), side["brain_id"])
try:
    rs.sign_receipt(src, key)
    c("refuses to sign a legacy unchained receipt", False, "signed")
except rs.SigError as e:
    c("refuses to sign a legacy unchained receipt", True, str(e))
bad = copy.deepcopy(recs)
bad[3]["token"] = (bad[3].get("token", 0) + 1) % 512
try:
    rs.sign_receipt(bad, key)
    c("refuses to sign a receipt that fails the core check", False, "signed")
except rs.SigError as e:
    c("refuses to sign a receipt that fails the core check", True, str(e))

N = 2000
t0 = time.perf_counter()
for _ in range(N):
    stepstream.close(copy.deepcopy(body))
t1 = time.perf_counter()
for _ in range(N):
    rs.close_and_sign(copy.deepcopy(body), key, timestamp=0)
t2 = time.perf_counter()
base_us, signed_us = (t1 - t0) / N * 1e6, (t2 - t1) / N * 1e6
print(f"  close: {base_us:.1f} us/receipt; close+sign: {signed_us:.1f} us/receipt "
      f"(+{signed_us - base_us:.1f} us, 16-step stream, {N} runs)")
c("signing overhead under 5 ms per receipt", signed_us - base_us < 5000)
c.finish("SIGN AT CLOSE (S1.4)")
