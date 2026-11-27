#!/usr/bin/env python3
"""ARC OS ring-3 exception isolation QEMU gate.

Runs the ARC_EXCEPTION_TEST shell hook. Each command forks a child that
triggers #DE, #UD, or #PF in ring 3. The gate requires the parent shell to
collect the child with the expected 128+vector status, print its per-case
isolation marker, and keep accepting commands afterward.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_TEST = os.path.join(ROOT, "tools", "boot_test.py")


def main():
    cmd = [
        sys.executable, BOOT_TEST,
        "--timeout", "30",
        "--settle", "1",
        "--expect", "arc> ",
        "--expect", "USERFAULT DE begin",
        "--expect", "USERFAULT DE child faulting",
        "--expect", "USERFAULT DE status 128",
        "--expect", "USERFAULT DE isolated",
        "--expect", "USERFAULT DE shell alive",
        "--expect", "USERFAULT UD begin",
        "--expect", "USERFAULT UD child faulting",
        "--expect", "USERFAULT UD status 134",
        "--expect", "USERFAULT UD isolated",
        "--expect", "USERFAULT UD shell alive",
        "--expect", "USERFAULT PF begin",
        "--expect", "USERFAULT PF child faulting",
        "--expect", "USERFAULT PF status 142",
        "--expect", "USERFAULT PF isolated",
        "--expect", "USERFAULT PF shell alive",
        "--expect-not", "PANIC",
        "--expect-not", "EXCEPTION",
        "--expect-not", "FAILED:",
        "--expect-not", "bad status",
        "--input", "faultde",
        "--input", "faultud",
        "--input", "faultpf",
        "--input", "exit",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
