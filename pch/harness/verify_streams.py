"""Verify the SHIPPED step-streams, read-only (release-package wrapper).

Loads stream_sampled.jsonl and stream_greedy.jsonl from this directory and
replays each against a freshly built fixture model. Nothing is regenerated
or overwritten: this is the stranger's first check -- do the sealed records
in this repo verify, exactly as shipped?

For the full battery (fresh runs, tamper cases, rewind/branch, witnessed
replay), run disc_test_llm.py and pch_more_tests.py afterwards.
"""
import os
import sys

import harness
import stepstream

_HERE = os.path.dirname(os.path.abspath(__file__))
STREAMS = ["stream_sampled.jsonl", "stream_greedy.jsonl"]

fails = 0
for name in STREAMS:
    path = os.path.join(_HERE, name)
    records = stepstream.load_stream(path)
    fresh = harness.build_model()
    ok, why = harness.verify_replay(fresh, records)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {why}")
    if not ok:
        fails += 1

print(f"\nSHIPPED STREAMS: {'ALL VERIFY' if fails == 0 else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
