#!/usr/bin/env python3
"""ARC OS deliberate ring-0 #UD gate.

Runs the ARC_RING0_FAULT_TEST kernel/shell hook. The shell issues a syscall
while it is the scheduled user process, but the fault is executed in kernel
mode. The gate requires the kernel fatal diagnostic and forbids every marker
of the user-process termination path. This is a separate crashing build; the
normal boot suite never runs it.
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
        "--expect", "RING0FAULT invoked from ring 3",
        "--expect", "RING0FAULT kernel UD2 vector=6",
        "--expect", "=== KERNEL EXCEPTION ===",
        "--expect", "#UD invalid opcode",
        "--expect", "Mode: kernel",
        "--expect", "System halted",
        "--expect-not", "[User fault]",
        "--expect-not", "terminating process",
        "--expect-not", "USERFAULT",
        "--expect-not", "FAILED:",
        "--input", "ring0fault",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
