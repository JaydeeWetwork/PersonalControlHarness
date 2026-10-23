"""Shared helpers for the PCH test beds. Exit 0 = pass, 1 = fail, 77 = skipped."""
import importlib.util
import os
import sys

SKIP = 77
HARNESS_DIR = os.environ.get(
    "PCH_HARNESS_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pch", "harness"))
sys.path.insert(0, os.path.abspath(HARNESS_DIR))


def skip(reason):
    print(f"SKIPPED: {reason}")
    sys.exit(SKIP)


def model_prereqs_or_skip():
    """Model-backed beds need torch and the DeepSeek-V3 specimen (not shipped)."""
    if importlib.util.find_spec("torch") is None:
        skip("torch not installed (pch/requirements.txt: torch==2.14.1+cpu; "
             "receipted on Python 3.12)")
    spec = os.environ.get("PCH_SPECIMEN_DIR",
                          os.path.join(HARNESS_DIR, "..", "DeepSeek-V3", "inference"))
    if not os.path.isfile(os.path.join(spec, "model.py")):
        skip(f"DeepSeek-V3 specimen not found at {os.path.abspath(spec)} "
             "(set PCH_SPECIMEN_DIR; see pch/README.md)")


class Checks:
    def __init__(self):
        self.fails = 0

    def __call__(self, name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
        if not ok:
            self.fails += 1

    def finish(self, title):
        print(f"\n{title}: {'ALL PASS' if not self.fails else str(self.fails) + ' FAILED'}")
        sys.exit(1 if self.fails else 0)
