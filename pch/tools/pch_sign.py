"""pch-sign: sign an already-closed schema-2 receipt (writes <receipt>.sig).

  python pch/tools/pch_sign.py <receipt.jsonl> <key.key> [--out FILE] [--timestamp N]

Passphrase-protected keys: set PCH_KEY_PASSPHRASE. Prefer signing at close
(harness.run_live_signed); this tool is for receipts closed elsewhere."""
import argparse
import os
import sys

import _common  # noqa: F401
import receipt_sig as rs
import stepstream


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pch-sign")
    ap.add_argument("receipt")
    ap.add_argument("key")
    ap.add_argument("--out")
    ap.add_argument("--timestamp", type=int)
    a = ap.parse_args(argv)
    pw = os.environ.get("PCH_KEY_PASSPHRASE")
    try:
        key = rs.load_private_key(a.key, pw.encode() if pw else None)
        side = rs.sign_receipt(stepstream.load_stream(a.receipt), key, a.timestamp)
    except (OSError, ValueError, TypeError) as e:
        print(f"ERROR: {e.__class__.__name__}: {e}")
        return 1
    out = a.out or a.receipt + ".sig"
    rs.save_sidecar(side, out)
    print(f"signed: {out} (key {side['key_id']}, brain {side['brain_id']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
