#!/usr/bin/env python3
"""ARC OS deliberate ring-0 #UD gate for full panic diagnostics.

Runs the ARC_PANIC_DIAG_TEST `panicud` hook. The shell issues a syscall while
it is the scheduled user process, but UD2 executes in ring 0. The gate
requires the centralized fatal report with RIP/CS/RFLAGS, all GPRs, explicit
ring-0 stack availability and PID, and forbids the ring-3 isolation path.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_TEST = os.path.join(ROOT, "tools", "boot_test.py")


def main():
    expectations = [
        "arc> ",
        "PANICUD invoked from ring 3",
        "PANICUD kernel UD2 vector=6",
        "=== KERNEL PANIC ===",
        "=== KERNEL EXCEPTION ===",
        "#UD invalid opcode",
        "Mode: kernel",
        "pid=",
        "interrupted rsp/ss unavailable (ring 0, no IST)",
        "handler stack=0x",
        "rip=0x",
        "cs=0x",
        "rflags=0x",
        "Registers:",
    ]
    expectations += ["%s=0x" % r for r in (
        "r15", "r14", "r13", "r12", "rbp", "rbx", "r11", "r10", "r9",
        "r8", "rdi", "rsi", "rdx", "rcx", "rax")]
    expectations.append("System halted")

    cmd = [sys.executable, BOOT_TEST,
           "--timeout", "30",
           "--settle", "1",
           "--allow-fatal"]
    for marker in expectations:
        cmd += ["--expect", marker]
    cmd += [
        "--expect-not", "[User fault]",
        "--expect-not", "terminating process",
        "--expect-not", "PANICREC",
        "--expect-not", "FAILED:",
        "--input", "panicud",
    ]
    print("+ " + " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
