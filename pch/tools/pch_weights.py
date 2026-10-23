"""pch-weights: show and check where the PCH will look for model weights.

  python pch/tools/pch_weights.py [--weights PATH]

Precedence: --weights, then PCH_WEIGHTS_DIR, then "weights_dir" in a config
file (PCH_CONFIG, ./pch.config.json, or the per-user pch.config.json), then
<repo>/models/weights. Exit 0 = a usable weights folder was found, 1 = the
chosen folder is missing or incomplete (the message says what is expected),
2 = usage error. Never downloads anything. Weights are separate from
PCH_SPECIMEN_DIR, which points at the specimen's code, not its weights."""
import argparse
import sys

import _common  # noqa: F401  (path setup)
import weights


def main(argv=None):
    ap = argparse.ArgumentParser(prog="pch-weights", description=__doc__.split("\n\n")[0],
                                 epilog="Paths with spaces must be quoted in the shell.")
    ap.add_argument("--weights", metavar="PATH", help="weights folder (highest precedence)")
    a = ap.parse_args(argv)
    try:
        path, source = weights.resolve(a.weights)
        print(f"weights folder: {path}")
        print(f"chosen by:      {source}")
        files = weights.validate(path, source)
    except weights.WeightsError as e:
        print(f"pch-weights: error: {e}", file=sys.stderr)
        return 1
    print(f"ok: {len(files)} weight file(s): {', '.join(files[:4])}{' ...' if len(files) > 4 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
