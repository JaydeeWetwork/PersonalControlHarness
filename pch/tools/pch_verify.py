"""pch-verify: offline receipt verification with only a public key (S1.5).

  python pch/tools/pch_verify.py <receipt.jsonl> <key.pub> [--sig FILE] [--revoked FILE]

Exit 0 = valid (chain, chained trailer, Ed25519 signature, key not revoked),
1 = rejected (the reason is printed), 2 = usage error. Does not run the
harness or the model and needs no network."""
import argparse
import sys

import _common  # noqa: F401  (path setup)
import receipt_sig as rs
import stepstream


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pch-verify", description=__doc__.split("\n\n")[0])
    ap.add_argument("receipt")
    ap.add_argument("pubkey")
    ap.add_argument("--sig", help="sidecar path (default: <receipt>.sig)")
    ap.add_argument("--revoked", help="revocation list (pch-revocations/1 JSON)")
    a = ap.parse_args(argv)
    try:
        pub = rs.load_public_key(a.pubkey)
        with open(a.receipt, "r", encoding="utf-8") as f:
            text = f.read()
        records = []
        for i, line in enumerate(text.splitlines()):
            if line.strip():
                r = stepstream.loads_strict(line)
                if not isinstance(r, dict):
                    raise rs.SigError(f"receipt: line {i} is not an object")
                records.append(r)
        with open(a.sig or a.receipt + ".sig", "r", encoding="utf-8") as f:
            side = f.read()
        revoked = set()
        if a.revoked:
            with open(a.revoked, "r", encoding="utf-8") as f:
                revoked = rs.parse_revocations(f.read())
        ok, why = rs.verify_receipt(records, side, pub, revoked)
    except (OSError, ValueError, RecursionError) as e:
        ok, why = False, f"{e.__class__.__name__}: {e}"
    print(("OK: " if ok else "REJECT: ") + why)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
