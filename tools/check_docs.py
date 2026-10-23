#!/usr/bin/env python3
"""Docs-vs-reality check (D4.5). Exit 0 = consistent, 1 = drift found.

  1. Hash pins (docs/pins.json): each pin appears in its files; no retired
     pin reappears where it is forbidden.
  2. README "Planned:" items: every bed marked Planned in the README table has
     a `planned` row in docs/CLAIMS.md naming that bed, and vice versa; every
     other "Planned:" line links DECISIONS.md or BLOCKERS.md.
  3. Every ctest named in docs/CLAIMS.md is registered in testbeds/CMakeLists.txt.
  4. testbeds/DECISIONS.md has a row for every bed 01-14.
  5. Test-bed status in README.md and testbeds/README.md matches the
     DECISIONS.md state, and every ctest DECISIONS/CLAIMS cite is registered.
Usage: check_docs.py [REPO_ROOT]
"""
import json
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
errors = []


def read(rel):
    p = os.path.join(ROOT, rel)
    if not os.path.exists(p):
        errors.append(f"missing file: {rel}")
        return ""
    with open(p, encoding="utf-8") as f:
        return f.read()


def table_rows(text):
    return [[c.strip() for c in ln.strip().strip("|").split("|")]
            for ln in text.splitlines() if ln.startswith("|") and not ln.startswith("|--")]


# 1. pins
pins_txt = read("docs/pins.json")
if pins_txt:
    pins = json.loads(pins_txt)
    for pin in pins.get("pins", []):
        for f in pin["files"]:
            if pin["value"] not in read(f):
                errors.append(f"pin drift: {pin['name']} {pin['value']} not found in {f}")
    for r in pins.get("retired", []):
        for f in r["forbidden_in"]:
            if r["value"] in read(f):
                errors.append(f"retired pin {r['value']} still present in {f} ({r['reason']})")

readme = read("README.md")
claims = read("docs/CLAIMS.md")
decisions = read("testbeds/DECISIONS.md")
cmake = read("testbeds/CMakeLists.txt")

# 2. Planned
readme_planned_beds = set()
for row in table_rows(readme):
    if len(row) >= 5 and re.fullmatch(r"\d\d", row[0]) and row[-1].startswith("Planned:"):
        readme_planned_beds.add(row[0])
claims_planned_beds = set()
for row in table_rows(claims):
    if len(row) >= 4 and row[-1].startswith("planned"):
        m = re.search(r"\(bed (\d\d)\)", row[1])
        if m:
            claims_planned_beds.add(m.group(1))
for b in sorted(readme_planned_beds - claims_planned_beds):
    errors.append(f"README marks bed {b} Planned but CLAIMS.md has no planned row for it")
for b in sorted(claims_planned_beds - readme_planned_beds):
    errors.append(f"CLAIMS.md says bed {b} is planned but the README table does not mark it Planned:")
for ln in readme.splitlines():
    if "Planned:" in ln and not ln.startswith("|") and not re.search(r"DECISIONS|BLOCKERS", ln):
        # allow the link on the following wrapped line
        i = readme.find(ln)
        nxt = readme[i:i + len(ln) + 120]
        if not re.search(r"DECISIONS|BLOCKERS", nxt):
            errors.append(f"README 'Planned:' item without a DECISIONS/BLOCKERS link: {ln.strip()[:80]}")
if "Planned:" in readme and "planned" not in claims:
    errors.append("README has Planned: items but CLAIMS.md has no planned rows")

# 3. ctest names in CLAIMS exist
registered = set(re.findall(r"pch_testbed\((pch\.[\w.]+)", cmake))
registered |= {f"pch.boss.{t}" for grp in re.findall(r"foreach\(tb ([^)]*)\)", cmake) for t in grp.split()}
for name in sorted(set(re.findall(r"\bpch\.[a-z0-9_]+(?:\.[a-z0-9_]+)?", claims))):
    if name not in registered and not name.startswith("pch.docs_check"):
        errors.append(f"CLAIMS.md cites ctest {name}, which is not registered")

# 4. DECISIONS covers 01-14
dec_rows = {r[0]: r for r in table_rows(decisions) if r and re.fullmatch(r"\d\d", r[0])}
for n in range(1, 15):
    b = f"{n:02d}"
    if b not in dec_rows:
        errors.append(f"DECISIONS.md has no row for bed {b}")
    elif dec_rows[b][3] not in ("Implement", "Retire"):
        errors.append(f"DECISIONS.md bed {b}: decision must be Implement or Retire, got {dec_rows[b][3]!r}")

# 5. README tables match the DECISIONS state (and so ctest): an implemented
#    bed must read as run/verified, a skipped bed must read Planned:/skipped.
def bed_status_rows(text):
    for row in table_rows(text):
        m = re.search(r"examples/(\d\d)-", "|".join(row))
        if m:
            yield m.group(1), row[-1]
for doc in ("README.md", "testbeds/README.md"):
    for b, status in bed_status_rows(read(doc)):
        row = dec_rows.get(b)
        if not row or len(row) < 5:
            continue
        implemented = row[4].startswith("implemented")
        says_run = bool(re.match(r"(Ran|verified)", status))
        if implemented != says_run:
            errors.append(f"{doc} bed {b} says {status[:40]!r} but DECISIONS.md state is {row[4]!r}")
for r in dec_rows.values():
    if len(r) > 2 and r[2] not in registered:
        errors.append(f"DECISIONS.md cites ctest {r[2]}, which is not registered")
for r in table_rows(claims):
    if len(r) >= 4 and r[-1].startswith("verified"):
        for t in re.findall(r"\bpch\.[a-z0-9_.]+", r[2]):
            if t.rstrip(".") not in registered:
                errors.append(f"CLAIMS {r[0]} verified by unregistered ctest {t}")

# Rule 6: every crew stays credited (JD, round 2 item 8) in both README and CREDITS.md.
CREW = ["JaydeeWetwork", "Cortana", "CloudAtlas", "Herominde", "Master Chief", "Elfie",
        "Muse team", "Archie", "DeepSeek team", "DeepSeek model", "Jenny", "Grok Bot team", "The consortium"]
for doc in ("README.md", "CREDITS.md"):
    text = re.sub(r"\s+", " ", read(doc))
    for name in CREW:
        if name not in text:
            errors.append(f"{doc} no longer credits {name}")

if errors:
    print("DOCS CHECK: FAIL")
    for e in errors:
        print("  [FAIL]", e)
    sys.exit(1)
print(f"DOCS CHECK: PASS (pins, {len(readme_planned_beds)} planned beds, "
      f"{len(registered)} ctests, {len(dec_rows)} decisions)")
