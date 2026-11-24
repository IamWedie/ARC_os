#!/usr/bin/env python3
"""ARC OS SYS_READ/SYS_WRITE user-pointer QEMU gate.

Runs the existing serial boot harness against the ARC_SYSCALL_TEST shell hook.
Two cases are checked in one boot:

  * negative: a kernel virtual address used as a user buffer must be rejected
    by both SYS_WRITE and SYS_READ, and the shell must keep running;
  * positive: a valid ring-3 stack buffer passed to SYS_READ must receive the
    bytes sent by the harness, and SYS_WRITE must echo those exact bytes back.
    The readok hook issues a bulk read so the kernel copies more than one byte
    through copy_to_user() in a single call; "bulk copy ok" asserts that.

The harness sends each --input line ~0.2 s apart, so the second line supplies
the payload the readok hook blocks on rather than a shell command.
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
        "--expect", "SYSCALL READ OK TEST",
        "--expect", "read 6",
        "--expect", "bulk copy ok",
        "--expect", "got READOK",
        "--expect", "SYSCALL PTR TEST",
        "--expect", "write -1",
        "--expect", "read -1",
        "--expect", "shell alive",
        "--expect-not", "PANIC",
        "--expect-not", "EXCEPTION",
        "--expect-not", "FAILED:",
        "--input", "readok",
        "--input", "READOK",
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
