#!/usr/bin/env python3
"""ARC OS SYS_READ/SYS_WRITE user-pointer QEMU gate.

Runs the existing serial boot harness against the ARC_SYSCALL_TEST shell hook.
The shell must reject a kernel virtual address passed as a user buffer to both
SYS_WRITE and SYS_READ, then continue accepting commands.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_TEST = os.path.join(ROOT, "tools", "boot_test.py")


def main():
    cmd = [
        sys.executable, BOOT_TEST,
        "--timeout", "20",
        "--settle", "1",
        "--expect", "arc> ",
        "--expect", "SYSCALL PTR TEST",
        "--expect", "write -1",
        "--expect", "read -1",
        "--expect", "shell alive",
        "--expect-not", "PANIC",
        "--expect-not", "EXCEPTION",
        "--expect-not", "FAILED:",
        "--input", "ptrtest",
    ]
    print("+ " + " ".join(cmd), flush=True)
    # The RED case echoes raw kernel bytes over serial. Decode them as UTF-8
    # with replacement instead of crashing the harness on Windows cp1252.
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
