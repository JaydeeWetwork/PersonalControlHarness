"""pch-keygen: make an Ed25519 key pair for signing PCH receipts (S1.6).

  python pch/tools/pch_keygen.py [--out-dir DIR] [--name NAME]

Writes NAME.key (PKCS#8 PEM, private; encrypted when PCH_KEY_PASSPHRASE is
set) and NAME.pub (SubjectPublicKeyInfo PEM, share freely). Default folder:
~/.pch/keys (outside any repository). Refuses to write a key inside a git
working tree, refuses to overwrite, restricts the private key file to the
current user, and prints only the key ID and paths, never key material.
See pch/docs/KEY-MANAGEMENT.md."""
import argparse
import os
import subprocess
import sys

import _common  # noqa: F401
import receipt_sig as rs


def inside_git_tree(path):
    p = os.path.abspath(path)
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return p
        parent = os.path.dirname(p)
        if parent == p:
            return None
        p = parent


def restrict(path):
    if os.name == "nt":
        user = os.environ.get("USERNAME", "")
        if user:
            subprocess.run(["icacls", path, "/inheritance:r", "/grant:r", f"{user}:F"],
                           capture_output=True, check=False)
    else:
        os.chmod(path, 0o600)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pch-keygen")
    ap.add_argument("--out-dir", default=os.path.join(os.path.expanduser("~"), ".pch", "keys"))
    ap.add_argument("--name", default="pch-receipts")
    a = ap.parse_args(argv)
    repo = inside_git_tree(a.out_dir)
    if repo:
        print(f"ERROR: {a.out_dir} is inside the git working tree {repo}; "
              "keep private keys out of repositories")
        return 1
    ser, ed = rs._crypto()
    os.makedirs(a.out_dir, exist_ok=True)
    kp, pp = (os.path.join(a.out_dir, a.name + ext) for ext in (".key", ".pub"))
    if os.path.exists(kp) or os.path.exists(pp):
        print(f"ERROR: {kp} or {pp} already exists; not overwriting")
        return 1
    key = ed.Ed25519PrivateKey.generate()
    pw = os.environ.get("PCH_KEY_PASSPHRASE")
    enc = ser.BestAvailableEncryption(pw.encode()) if pw else ser.NoEncryption()
    fd = os.open(kp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(ser.Encoding.PEM, ser.PrivateFormat.PKCS8, enc))
    restrict(kp)
    with open(pp, "wb") as f:
        f.write(key.public_key().public_bytes(ser.Encoding.PEM,
                                              ser.PublicFormat.SubjectPublicKeyInfo))
    print(f"key id {rs.key_id(rs.public_raw(key.public_key()))}: private {kp} "
          f"({'passphrase-encrypted' if pw else 'unencrypted; set PCH_KEY_PASSPHRASE to encrypt'}), "
          f"public {pp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
