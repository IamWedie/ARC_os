#!/usr/bin/env python3
"""ARC OS deliberate ring-0 #PF gate for CR2 and error-code decode.

Runs the ARC_PANIC_DIAG_TEST `panicpf` hook, which performs a real supervisor
write to an unmapped guest address. The gate requires CR2, the hardware error
code, architecturally meaningful decode fields and the kernel fatal path.
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
        "--expect", "PANICPF invoked from ring 3",
        "--expect", "PANICPF kernel write vector=14 "
                    "address=0x00000000dead0000",
        "--expect", "=== KERNEL PANIC ===",
        "--expect", "=== KERNEL EXCEPTION ===",
        "--expect", "#PF page fault",
        "--expect", "Mode: kernel",
        "--expect", "error=0x0000000000000002",
        "--expect", "cr2 (faulting address)=0x00000000dead0000",
        "--expect", "PF error=0x0000000000000002",
        "--expect", "P=0 not-present",
        "--expect", "W=1 write",
        "--expect", "U=0 supervisor",
        "--expect", "RSVD=0",
        "--expect", "I/D=0 data-access",
        "--expect", "PK=0",
        "--expect", "SS=0",
        "--expect", "SGX=0",
        "--expect", "System halted",
        "--expect-not", "[User fault]",
        "--expect-not", "terminating process",
        "--expect-not", "PANICUD",
        "--expect-not", "FAILED:",
        "--input", "panicpf",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
