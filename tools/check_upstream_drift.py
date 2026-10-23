"""Upstream drift check for third_party/DeepSeek-V3 (vendored, not a submodule).

Offline (default, runs in ctest and CI on every push):
  - UPSTREAM.md records a full 40-hex pinned commit, and it is the commit in
    docs/pins.json "vendored" (9b4e978...). Every doc that cites a short
    DeepSeek-V3 commit cites that one.
  - Every file on disk matches its sha256 in the UPSTREAM.md manifest, and
    no file is added or missing.
--online (CI nightly): also downloads each manifest path from GitHub at the
  pinned commit and checks it hashes to the manifest value, so a wrong
  manifest (not just a modified file) is caught. Downloads code only, never
  weights. Network failure exits 77 (skip), never a fake pass.

Exit 0 no drift, 1 drift, 77 online check could not reach GitHub.
Code by the DeepSeek team (MIT). Built by Cortana; directed by JaydeeWetwork."""
import argparse
import hashlib
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VENDOR = os.path.join(ROOT, "third_party", "DeepSeek-V3")
DOCS_CITING = ["testbeds/README.md", "testbeds/SUMMARY.md", "README.md"]


def load():
    with open(os.path.join(VENDOR, "UPSTREAM.md"), encoding="utf-8") as f:
        up = f.read()
    m = re.search(r"Pinned commit: `([0-9a-f]{40})`", up)
    manifest = dict((p, h) for h, p in re.findall(r"^([0-9a-f]{64})  (\S+)$", up, re.M))
    return (m.group(1) if m else None), manifest


def offline(problems):
    commit, manifest = load()
    if not commit:
        problems.append("UPSTREAM.md: no full 40-hex 'Pinned commit'")
        return commit, manifest
    with open(os.path.join(ROOT, "docs", "pins.json"), encoding="utf-8") as f:
        pins = json.load(f)
    want = (pins.get("vendored") or {}).get("deepseek_v3_commit")
    if want != commit:
        problems.append(f"docs/pins.json vendored.deepseek_v3_commit is {want!r}, UPSTREAM.md pins {commit}")
    for rel in DOCS_CITING:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8") as f:
            for short in re.findall(r"DeepSeek-V3 `([0-9a-f]{7,40})`", f.read()):
                if not commit.startswith(short):
                    problems.append(f"{rel} cites DeepSeek-V3 `{short}`, pinned is {commit[:7]}")
    if not manifest:
        problems.append("UPSTREAM.md: empty manifest")
    on_disk = set()
    for d, _, files in os.walk(VENDOR):
        for n in files:
            rel = os.path.relpath(os.path.join(d, n), VENDOR).replace(os.sep, "/")
            if rel != "UPSTREAM.md" and "__pycache__" not in rel:
                on_disk.add(rel)
    for rel in sorted(on_disk - set(manifest)):
        problems.append(f"added, not in manifest: {rel}")
    for rel in sorted(set(manifest) - on_disk):
        problems.append(f"missing, in manifest: {rel}")
    for rel in sorted(on_disk & set(manifest)):
        with open(os.path.join(VENDOR, rel), "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != manifest[rel]:
                problems.append(f"modified vs manifest: {rel}")
    return commit, manifest


def online(commit, manifest, problems):
    base = f"https://raw.githubusercontent.com/deepseek-ai/DeepSeek-V3/{commit}/"
    for rel, h in sorted(manifest.items()):
        try:
            with urllib.request.urlopen(base + rel, timeout=30) as r:
                data = r.read()
        except OSError as e:
            print(f"SKIPPED: cannot reach upstream ({e.__class__.__name__}: {e})")
            return 77
        if hashlib.sha256(data).hexdigest() != h:
            problems.append(f"manifest disagrees with upstream {commit[:7]}: {rel}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fail if third_party/DeepSeek-V3 drifts from its pinned upstream commit.")
    ap.add_argument("--online", action="store_true", help="also compare against GitHub at the pinned commit")
    a = ap.parse_args(argv)
    problems = []
    commit, manifest = offline(problems)
    if a.online and commit and not problems:
        rc = online(commit, manifest, problems)
        if rc == 77 and not problems:
            return 77
    for p in problems:
        print(f"  [DRIFT] {p}")
    where = "offline+online" if a.online else "offline"
    print(f"UPSTREAM DRIFT CHECK ({where}): " + (f"{len(problems)} problem(s)" if problems else
          f"PASS ({len(manifest)} files, pinned {commit[:7] if commit else '?'})"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
