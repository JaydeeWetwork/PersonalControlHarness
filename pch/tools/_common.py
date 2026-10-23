"""Shared path setup for the PCH command-line tools."""
import os
import sys

HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness")
sys.path.insert(0, os.path.abspath(os.environ.get("PCH_HARNESS_DIR", HARNESS)))
