"""Regression tests for bugs found in the 2026-10-23 code review
(docs/reviews/PCH_CODE_REVIEW_2026-10-23_Cortana.md). Each case failed
before its fix. Built by: Cortana."""
import importlib.util
import json
import os
import tempfile

import pch_testlib
import stepstream

c = pch_testlib.Checks()


def body(n=3):
    return [{"kind": "header", "run_seed": 1}] + [{"kind": "step", "step": i, "token": i} for i in range(n)]


# R1: NaN/Infinity must never be hashed (loads_strict rejects them on load,
# so a NaN receipt could be closed and signed but never verified).
try:
    stepstream.close(body() + [{"kind": "event", "x": float("nan")}])
    c("R1 close() refuses a record holding NaN", False, "closed a NaN record")
except ValueError as e:
    c("R1 close() refuses a record holding NaN", True, str(e)[:50])

# R2: hostile nesting must be a clean rejection, not a crash.
deep = []
for _ in range(100000):
    deep = [deep]
recs = stepstream.close(body())
recs[1]["token"] = deep
try:
    ok, why, _ = stepstream.check_sealed(recs)
    c("R2 deeply nested record: clean reject, no crash", not ok, why)
except RecursionError:
    c("R2 deeply nested record: clean reject, no crash", False, "RecursionError escaped")

# R3: replay must reject a re-hashed stream whose step numbers are not 0..n-1
# (a duplicated or skipped step re-derived the same token and was accepted).
dup = body(3)
dup[3]["step"] = 1                      # steps 0,1,1
stepstream.close(dup)
ok, why = stepstream.check_step_sequence(dup) if hasattr(stepstream, "check_step_sequence") else (True, "no check")
c("R3 duplicated step number is rejected", not ok, why)
gap = body(3)
gap[3]["step"] = 5
ok, why = stepstream.check_step_sequence(stepstream.close(gap)) if hasattr(stepstream, "check_step_sequence") else (True, "")
c("R3 skipped step number is rejected", not ok, why)
ok, why = stepstream.check_step_sequence(stepstream.close(body(3))) if hasattr(stepstream, "check_step_sequence") else (False, "missing")
c("R3 contiguous steps 0..n-1 pass", ok, why)

# R4: save_stream writes UTF-8 with LF on every OS, so a stream's bytes (and
# file hashes) are identical on Windows and Linux.
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "s.jsonl")
    r = stepstream.close(body() + [{"kind": "event", "note": "caf\u00e9"}])
    stepstream.save_stream(r, p)
    raw = open(p, "rb").read()
    c("R4 saved stream uses LF only", b"\r\n" not in raw)
    c("R4 saved stream round-trips and stays sealed", stepstream.check_sealed(stepstream.load_stream(p))[0])

# R5: a revocation entry whose key_id is not 16 lowercase hex used to be
# accepted and then silently never matched (the key stayed trusted).
if importlib.util.find_spec("cryptography"):
    import receipt_sig as rs
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    k = Ed25519PrivateKey.generate()
    r = stepstream.close(body())
    side = rs.sign_receipt(r, k, 1)
    kid = side["key_id"]
    for label, bad in (("uppercase", kid.upper()), ("padded", " " + kid), ("short", kid[:8])):
        doc = json.dumps({"format": "pch-revocations/1", "revoked": [{"key_id": bad}]})
        try:
            rev = rs.parse_revocations(doc)
            ok, _ = rs.verify_receipt(r, side, k.public_key(), rev)
            c(f"R5 {label} key_id in a revocation list is refused, not ignored", False,
              "list accepted; receipt " + ("still verified" if ok else "rejected"))
        except rs.SigError as e:
            c(f"R5 {label} key_id in a revocation list is refused, not ignored", True, str(e))
else:
    c("R5 skipped: cryptography not installed", True)

c.finish("CODE REVIEW REGRESSIONS")
