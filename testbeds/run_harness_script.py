"""Run one of the shipped pch/harness scripts under CTest, skipping (77)
when the reference-model prerequisites are missing. Scripts are run as-is."""
import os
import runpy
import sys

import pch_testlib

pch_testlib.model_prereqs_or_skip()
script = os.path.join(pch_testlib.HARNESS_DIR, sys.argv[1])
os.chdir(pch_testlib.HARNESS_DIR)
sys.argv = [script]
runpy.run_path(script, run_name="__main__")
