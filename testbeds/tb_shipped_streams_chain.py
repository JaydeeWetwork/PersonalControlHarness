"""Chain-only check of the shipped sealed streams (no model needed): every
record links, the trailer seals the final hash, step counter is contiguous."""
import os

import pch_testlib
from receipt_check import Reject, parse, verify

c = pch_testlib.Checks()
for n in ["stream_sampled.jsonl", "stream_greedy.jsonl"]:
    with open(os.path.join(pch_testlib.HARNESS_DIR, n)) as f:
        try:
            head, steps = verify(parse(f.read()))
            c(f"{n} chain + seal intact", True, f"{steps} steps, head {head[:16]}")
        except Reject as e:
            c(f"{n} chain + seal intact", False, str(e))
c.finish("SHIPPED STREAMS (chain only)")
