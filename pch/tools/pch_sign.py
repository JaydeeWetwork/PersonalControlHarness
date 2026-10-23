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
    ap = argparse.ArgumentParser(prog="pch-sign", description="Sign a sealed PCH receipt (.jsonl) with an Ed25519 key; writes a pch-sig/1 sidecar. Exit 0 signed, 1 error.")
    ap.add_argument("receipt", help="sealed receipt .jsonl (must pass check_sealed)")
    ap.add_argument("key", help="Ed25519 private key PEM from pch-keygen (never commit it)")
    ap.add_argument("--out", help="sidecar path (default: <receipt>.sig)")
    ap.add_argument("--timestamp", type=int, help="signing time, unix seconds (default: now; set it for reproducible sidecars)")
    a = ap.parse_args(argv)
    pw = os.environ.get("PCH_KEY_PASSPHRASE")
    try:
        key = rs.load_private_key(a.key, pw.encode() if pw else None)
        side = rs.sign_receipt(stepstream.load_stream(a.receipt), key, a.timestamp)
    except (OSError, ValueError, TypeError) as e:
        print(f"ERROR: {e.__class__.__name__}: {e}", file=sys.stderr)
        return 1
    out = a.out or a.receipt + ".sig"
    rs.save_sidecar(side, out)
    print(f"signed: {out} (key {side['key_id']}, brain {side['brain_id']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
