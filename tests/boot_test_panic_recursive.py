#!/usr/bin/env python3
"""ARC OS deliberate recursive-panic gate.

The first kernel #UD produces a complete fatal report. Before the first path
halts, the test hook re-enters the centralized panic entry point; the guard
must reject it and go straight to cli/hlt with no second report and no nested
formatting. Marker-count limits prove the emergency path stayed silent.
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
        "--allow-fatal",
        "--expect", "arc> ",
        "--expect", "PANICREC invoked from ring 3",
        "--expect", "PANICREC arming recursive fatal entry",
        "--expect", "=== KERNEL PANIC ===",
        "--expect", "=== KERNEL EXCEPTION ===",
        "--expect", "#UD invalid opcode",
        "--expect", "Mode: kernel",
        "--expect", "System halted",
        "--expect", "PANICREC first panic report complete; recursive entry follows",
        "--expect-max", "=== KERNEL PANIC ===", "1",
        "--expect-max", "=== KERNEL EXCEPTION ===", "1",
        "--expect-max", "Registers:", "1",
        "--expect-max", "System halted", "1",
        "--expect-not", "reason: PANICREC recursive fatal entry",
        "--expect-not", "[User fault]",
        "--expect-not", "terminating process",
        "--expect-not", "FAILED:",
        "--input", "panicrecursive",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
