"""Verify the SHIPPED step-streams, read-only (release-package wrapper).

Picks the reference stream set recorded on a platform that matches this one
(OS, CPU arch, torch version) and replays each stream against a freshly
built fixture model. Nothing is regenerated or overwritten.

Reference sets (core fix approved by JD 2026-10-23):
  ./stream_*.jsonl                       the original Linux set. Its headers
                                         predate the platform tag; the
                                         platform is taken from
                                         LEGACY_PLATFORM below (receipted in
                                         requirements.txt).
  ./reference_streams/<name>/stream_*.jsonl
                                         tagged sets; the platform is read
                                         from each stream's hashed header.
If no set matches, this exits 77 (CTest: Skipped) with the reason. Float
results differ across platforms, so a mismatch there is not tampering.

For the full battery (fresh runs, tamper cases, rewind/branch, witnessed
replay), run disc_test_llm.py and pch_more_tests.py afterwards.
"""
import glob
import os
import sys

import harness
import stepstream

_HERE = os.path.dirname(os.path.abspath(__file__))
STREAMS = ["stream_sampled.jsonl", "stream_greedy.jsonl"]
LEGACY_PLATFORM = {"os": "Linux", "arch": "x86_64", "torch": "2.14.1+cpu",
                   "python": "3.12"}
SKIP = 77


def stream_sets():
    """Yield (label, dir, platform) for each reference set on disk."""
    yield "legacy (untagged, Linux)", _HERE, LEGACY_PLATFORM
    for d in sorted(glob.glob(os.path.join(_HERE, "reference_streams", "*"))):
        if not os.path.isdir(d):
            continue
        plats = []
        for name in STREAMS:
            hdr = stepstream.load_stream(os.path.join(d, name))[0]
            plats.append(hdr.get("platform"))
        if not plats[0] or any(harness.platform_key(p) != harness.platform_key(plats[0])
                               for p in plats):
            print(f"  [WARN] {os.path.basename(d)}: missing or inconsistent platform tags; ignored")
            continue
        yield os.path.basename(d), d, plats[0]


here = harness.platform_info()
key = harness.platform_key(here)
print(f"  this platform: {here['os']} {here['arch']}, torch {here['torch']}, python {here['python']}")
sets = list(stream_sets())
match = next((s for s in sets if harness.platform_key(s[2]) == key), None)
if match is None:
    print("  available reference sets: " + "; ".join(
        f"{lbl} = {p['os']} {p['arch']} torch {p['torch']}" for lbl, _, p in sets))
    print("\nSHIPPED STREAMS: SKIPPED -- no reference streams recorded on a matching "
          "platform (OS, arch, torch). Record a set with disc_test_llm.py and "
          "PCH_OUT_DIR=reference_streams/<name>.")
    sys.exit(SKIP)

label, d, _ = match
print(f"  using reference set: {label}")
fails = 0
for name in STREAMS:
    records = stepstream.load_stream(os.path.join(d, name))
    fresh = harness.build_model()
    ok, why = harness.verify_replay(fresh, records)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {why}")
    if not ok:
        fails += 1

print(f"\nSHIPPED STREAMS: {'ALL VERIFY' if fails == 0 else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
