"""S1.3: the receipt trailer is inside the hash chain (PCH core).

Before 0.0.0.4 the core check (harness.verify_replay) only re-hashed the
records BEFORE the trailer and compared final_chain, so trailer edits such as
an extra field or a wrong step count passed the core. This bed edits the
trailer of a schema-2 receipt in several ways and requires the core check to
reject every edit. Run against the pre-fix harness (PCH_HARNESS_DIR pointing
at an older pch/harness) it FAILS; with the fix it passes.
Designed by: CloudAtlas (trailer finding from bed 03). Built by: Cortana."""
import copy
import os

import pch_testlib
import stepstream


def core_check(records):
    """The PCH core's receipt check, whichever version is on the path."""
    if hasattr(stepstream, "check_sealed"):
        ok, why, _ = stepstream.check_sealed(records)
        return ok, why
    # pre-0.0.0.4 core (harness.verify_replay's chain + seal steps, no model)
    body = [r for r in records if r.get("kind") != "trailer"]
    if not stepstream.check_chain(body):
        return False, "hash chain broken"
    if records[-1].get("final_chain") != body[-1].get("chain"):
        return False, "trailer seal mismatch"
    return True, "accepted (old core)"


def reclose(recs):
    body = [copy.deepcopy(r) for r in recs if r.get("kind") != "trailer"]
    for r in body:
        r.pop("chain", None)
    if hasattr(stepstream, "close"):
        return stepstream.close(body)
    stepstream.seal(body)  # old core: unchained trailer
    n = sum(1 for r in body if r["kind"] == "step")
    return body + [{"kind": "trailer", "steps": n, "final_chain": body[-1]["chain"]}]


c = pch_testlib.Checks()
base = reclose(stepstream.load_stream(os.path.join(pch_testlib.HARNESS_DIR, "stream_sampled.jsonl")))
ok, why = core_check(base)
c("clean re-closed receipt accepted", ok, why)
c("trailer carries schema 2 and its own chain link",
  base[-1].get("schema") == 2 and "chain" in base[-1])

edits = {
    "extra trailer field": lambda t: t.__setitem__("note", "edited after close"),
    "step count edited": lambda t: t.__setitem__("steps", t.get("steps", 0) + 1),
    "schema downgraded (field removed)": lambda t: t.pop("schema", None),
    "trailer chain link edited": lambda t: t.__setitem__("chain", "0" * 64),
    "final_chain edited": lambda t: t.__setitem__("final_chain", "f" * 64),
}
for name, fn in edits.items():
    r = copy.deepcopy(base)
    fn(r[-1])
    ok, why = core_check(r)
    c(f"core rejects: {name}", not ok, why)

legacy = stepstream.load_stream(os.path.join(pch_testlib.HARNESS_DIR, "stream_sampled.jsonl"))
if hasattr(stepstream, "check_sealed"):
    ok, why, label = stepstream.check_sealed(legacy)
    c("shipped legacy stream still readable, labeled legacy-unchained",
      ok and label == "legacy-unchained", why)
c.finish("TRAILER IN CHAIN (S1.3)")
