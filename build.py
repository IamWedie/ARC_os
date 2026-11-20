#!/usr/bin/env python3
"""Thin entry point for the ARC OS full build (see tools/build_kernel.py)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_SCRIPT = os.path.join(ROOT, "tools", "build_kernel.py")

if __name__ == "__main__":
    sys.exit(subprocess.call([sys.executable, BUILD_SCRIPT] + sys.argv[1:]))
