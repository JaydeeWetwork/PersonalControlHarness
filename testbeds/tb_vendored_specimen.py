"""The vendored DeepSeek-V3 inference code is byte-for-byte the pinned
upstream commit: every file in third_party/DeepSeek-V3/UPSTREAM.md's manifest
exists with the recorded sha256, nothing extra is present, and LICENSE-CODE
(MIT) is there. Built by: Cortana. Code by the DeepSeek team (MIT)."""
import hashlib
import os
import re

import pch_testlib

ROOT = os.path.abspath(os.path.join(pch_testlib.HARNESS_DIR, "..", "..", "third_party", "DeepSeek-V3"))
c = pch_testlib.Checks()
with open(os.path.join(ROOT, "UPSTREAM.md"), encoding="utf-8") as f:
    up = f.read()
c("pinned commit recorded", "9b4e9788e4a3a731f7567338ed15d3ec549ce03b" in up)
manifest = dict((p, h) for h, p in re.findall(r"^([0-9a-f]{64})  (\S+)$", up, re.M))
c("manifest lists the code and LICENSE-CODE", "inference/model.py" in manifest and "LICENSE-CODE" in manifest)
on_disk = set()
for d, _, files in os.walk(ROOT):
    for n in files:
        rel = os.path.relpath(os.path.join(d, n), ROOT).replace(os.sep, "/")
        if rel != "UPSTREAM.md" and "__pycache__" not in rel:
            on_disk.add(rel)
c("no files added or missing vs the manifest", on_disk == set(manifest),
  f"extra {sorted(on_disk - set(manifest))}, missing {sorted(set(manifest) - on_disk)}")
for rel, h in sorted(manifest.items()):
    with open(os.path.join(ROOT, rel), "rb") as f:
        data = f.read()
    c(f"unmodified: {rel}", hashlib.sha256(data).hexdigest() == h)
with open(os.path.join(ROOT, "LICENSE-CODE"), encoding="utf-8") as f:
    lic = f.read()
c("LICENSE-CODE is the MIT license, (c) 2023 DeepSeek", "MIT License" in lic and "2023 DeepSeek" in lic)
c.finish("VENDORED DEEPSEEK-V3 CODE (pinned, unmodified)")
