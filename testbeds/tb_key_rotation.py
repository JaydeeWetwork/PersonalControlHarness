"""Key rotation and revocation, exercised end to end as pch/docs/KEY-MANAGEMENT.md
describes them, through the real CLIs (pch-keygen, pch-sign, pch-verify):

1. Year-1 key signs receipts; rotate: generate a year-2 key (purpose-year name).
2. Old receipts stay verifiable with the old public key after rotation (policy:
   "rotation does not invalidate old receipts"); new receipts verify with the
   new key; neither verifies with the other's key; each .sig names its key_id.
3. Revoke the year-1 key (pch-revocations/1): every year-1 receipt is rejected,
   year-2 receipts still verify, and the reason names the revoked key.
4. Revocation is all-or-nothing per key: a back-dated year-1 signature
   (--timestamp before revoked_at) is still rejected.
5. A passphrase-encrypted year-2 key (PCH_KEY_PASSPHRASE) signs; without the
   passphrase pch-sign fails cleanly.
6. pch-keygen refuses to overwrite a key and refuses a folder inside a git tree.
Keys live only in an OS temp folder. Designed from KEY-MANAGEMENT.md;
built by Cortana; directed by JaydeeWetwork."""
import json
import os
import subprocess
import sys
import tempfile

import pch_testlib
import stepstream

TOOLS = os.path.abspath(os.path.join(pch_testlib.HARNESS_DIR, "..", "tools"))
REPO = os.path.abspath(os.path.join(pch_testlib.HARNESS_DIR, "..", ".."))
c = pch_testlib.Checks()
try:
    import cryptography  # noqa: F401
except ImportError:
    print("SKIPPED: the 'cryptography' package is not installed (pip install -r pch/requirements.txt)")
    sys.exit(77)


def run(tool, *args, env=None):
    e = dict(os.environ)
    e.pop("PCH_KEY_PASSPHRASE", None)
    e.update(env or {})
    p = subprocess.run([sys.executable, os.path.join(TOOLS, tool), *args], capture_output=True,
                       text=True, timeout=120, env=e)
    return p.returncode, (p.stdout + p.stderr).strip()


def receipt(path, seed):
    body = [{"kind": "header", "run_seed": seed, "brain": "rotation-test"}] + \
           [{"kind": "step", "step": i, "token": (seed * 31 + i) % 97} for i in range(8)]
    stepstream.save_stream(stepstream.close(body), path)
    return path


def sig_key(path):
    with open(path + ".sig", encoding="utf-8") as f:
        return json.load(f)["key_id"]


with tempfile.TemporaryDirectory(prefix="pch-rotate-") as tmp:
    keys = os.path.join(tmp, "keys")
    rc, out = run("pch_keygen.py", "--out-dir", keys, "--name", "boss-receipts-2026")
    c("1 year-1 key generated", rc == 0, out)
    k1, p1 = os.path.join(keys, "boss-receipts-2026.key"), os.path.join(keys, "boss-receipts-2026.pub")
    old = [receipt(os.path.join(tmp, f"old{i}.jsonl"), i) for i in range(3)]
    for r in old:
        run("pch_sign.py", r, k1, "--timestamp", "1767225600")       # 2026-01-01
    rc, out = run("pch_keygen.py", "--out-dir", keys, "--name", "boss-receipts-2027")
    c("1 rotation: year-2 key generated next to year-1", rc == 0, out)
    k2, p2 = os.path.join(keys, "boss-receipts-2027.key"), os.path.join(keys, "boss-receipts-2027.pub")
    new = [receipt(os.path.join(tmp, f"new{i}.jsonl"), 100 + i) for i in range(3)]
    for r in new:
        run("pch_sign.py", r, k2, "--timestamp", "1798761600")       # 2027-01-01
    id1, id2 = sig_key(old[0]), sig_key(new[0])
    c("2 each .sig names its key_id; ids differ across the rotation", id1 != id2 and
      all(sig_key(r) == id1 for r in old) and all(sig_key(r) == id2 for r in new), f"{id1} -> {id2}")
    c("2 old receipts still verify with the old public key after rotation",
      all(run("pch_verify.py", r, p1)[0] == 0 for r in old))
    c("2 new receipts verify with the new public key", all(run("pch_verify.py", r, p2)[0] == 0 for r in new))
    c("2 old receipts do not verify with the new key", all(run("pch_verify.py", r, p2)[0] == 1 for r in old))
    c("2 new receipts do not verify with the old key", all(run("pch_verify.py", r, p1)[0] == 1 for r in new))

    rev = os.path.join(tmp, "revoked.json")
    with open(rev, "w", encoding="utf-8") as f:
        json.dump({"format": "pch-revocations/1", "revoked": [
            {"key_id": id1, "revoked_at": "2026-10-23", "reason": "laptop lost"}]}, f)
    res = [run("pch_verify.py", r, p1, "--revoked", rev) for r in old]
    c("3 after revocation every year-1 receipt is rejected", all(rc == 1 for rc, _ in res))
    c("3 the rejection names the revoked key", all(id1 in o or "revoked" in o.lower() for _, o in res), res[0][1][:90])
    c("3 year-2 receipts still verify with the same revocation list",
      all(run("pch_verify.py", r, p2, "--revoked", rev)[0] == 0 for r in new))

    back = receipt(os.path.join(tmp, "backdated.jsonl"), 7)
    run("pch_sign.py", back, k1, "--timestamp", "946684800")          # claims 2000-01-01
    c("4 a back-dated year-1 signature is still rejected (all-or-nothing per key)",
      run("pch_verify.py", back, p1, "--revoked", rev)[0] == 1)
    c("4 ...and it verifies without the list (proves the list did the rejecting)",
      run("pch_verify.py", back, p1)[0] == 0)

    pw = {"PCH_KEY_PASSPHRASE": "rotation-test-passphrase"}
    rc, out = run("pch_keygen.py", "--out-dir", keys, "--name", "boss-receipts-2027-enc", env=pw)
    c("5 passphrase-encrypted key generated", rc == 0, out)
    ke, pe = os.path.join(keys, "boss-receipts-2027-enc.key"), os.path.join(keys, "boss-receipts-2027-enc.pub")
    with open(ke, "rb") as f:
        c("5 private key file is encrypted PKCS#8", b"ENCRYPTED" in f.read())
    enc = receipt(os.path.join(tmp, "enc.jsonl"), 9)
    rc, out = run("pch_sign.py", enc, ke)
    c("5 without the passphrase pch-sign fails cleanly (exit 1, no traceback)",
      rc == 1 and "Traceback" not in out, out[:80])
    c("5 with the passphrase it signs and verifies",
      run("pch_sign.py", enc, ke, env=pw)[0] == 0 and run("pch_verify.py", enc, pe)[0] == 0)

    rc, out = run("pch_keygen.py", "--out-dir", keys, "--name", "boss-receipts-2026")
    c("6 pch-keygen refuses to overwrite an existing key", rc == 1, out[:70])
    rc, out = run("pch_keygen.py", "--out-dir", os.path.join(REPO, "build-keys-should-not-exist"),
                  "--name", "x")
    c("6 pch-keygen refuses a folder inside the git working tree", rc == 1 and
      not os.path.exists(os.path.join(REPO, "build-keys-should-not-exist")), out[:70])

c.finish("KEY ROTATION AND REVOCATION (KEY-MANAGEMENT.md)")
