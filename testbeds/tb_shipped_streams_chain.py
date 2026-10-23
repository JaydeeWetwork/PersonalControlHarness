"""Chain-only check of the shipped sealed streams (no model needed): every
record links, the trailer seals the final hash, step counter is contiguous.

Covers the two streams in pch/harness/ and every recorded set under
pch/harness/reference_streams/<platform>/."""
import os

import pch_testlib
from receipt_check import Reject, parse, verify

STREAMS = ["stream_sampled.jsonl", "stream_greedy.jsonl"]


def stream_files():
    """Yield (label, path) for the top-level streams and each reference set."""
    for n in STREAMS:
        yield n, os.path.join(pch_testlib.HARNESS_DIR, n)
    ref_root = os.path.join(pch_testlib.HARNESS_DIR, "reference_streams")
    if os.path.isdir(ref_root):
        for set_name in sorted(os.listdir(ref_root)):
            set_dir = os.path.join(ref_root, set_name)
            if not os.path.isdir(set_dir):
                continue
            for n in STREAMS:
                p = os.path.join(set_dir, n)
                if os.path.exists(p):
                    yield f"reference_streams/{set_name}/{n}", p


c = pch_testlib.Checks()
for label, path in stream_files():
    with open(path, encoding="utf-8") as f:
        try:
            head, steps = verify(parse(f.read()))
            c(f"{label} chain + seal intact", True, f"{steps} steps, head {head[:16]}")
        except Reject as e:
            c(f"{label} chain + seal intact", False, str(e))
c.finish("SHIPPED STREAMS (chain only)")
