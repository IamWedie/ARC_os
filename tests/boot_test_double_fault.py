#!/usr/bin/env python3
"""ARC OS deliberate ring-0 double-fault / IST gate.

Runs the ARC_DOUBLE_FAULT_TEST kernel/shell hook. The shell invokes a syscall
while it is the scheduled user process, but the hook deliberately makes the
ring-0 stack unusable before executing UD2. The failed #UD delivery must become
#DF, which the CPU enters through TSS IST1. This gate requires the kernel fatal
diagnostic, the dedicated-IST confirmation, and forbids every marker of the
user-process termination path. It is a separate crashing build; the normal
boot suite never runs it.
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
        "--expect", "DOUBLEFAULT invoked from ring 3",
        "--expect", "DOUBLEFAULT unusable ring0 stack -> #DF vector=8",
        "--expect", "=== KERNEL EXCEPTION ===",
        "--expect", "#DF double fault",
        "--expect", "Mode: kernel",
        "--expect", "DOUBLEFAULT frame on dedicated IST stack",
        "--expect", "System halted",
        "--expect-not", "DOUBLEFAULT frame NOT on dedicated IST stack",
        "--expect-not", "[User fault]",
        "--expect-not", "terminating process",
        "--expect-not", "USERFAULT",
        "--expect-not", "RING0FAULT",
        "--expect-not", "FAILED:",
        "--input", "doublefault",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
