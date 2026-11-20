#!/usr/bin/env python3
"""
ARC OS boot smoke test.

Boots os.elf in QEMU, captures the serial console over a TCP chardev, waits
for expected markers, and fails on a panic/fault, a missing marker, or a
forbidden marker instead of hanging forever.
This is the runtime quality gate that complements the host slab tests and the
build-time image checks.

Why TCP instead of `-serial stdio`?  QEMU's Windows stdio backend reads stdin
one byte at a time and discards the byte when the emulated 16550 advertises
zero receive capacity (see chardev/char-win-stdio.c).  A piped shell command
is therefore truncated as soon as the FIFO trigger is reached.  The TCP
chardev applies backpressure instead: QEMU stops reading the socket until the
guest has drained the UART, so every byte survives.  Interactive use can still
use `-serial stdio`; human typing is slow enough not to hit the drop path.

Usage:
    python tools/boot_test.py
    python tools/boot_test.py --timeout 20 --expect "arc> "
    python tools/boot_test.py --expect "beta" --expect-not "alphabeta"
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")
DEFAULT_MARKERS = ["=== ARC OS v1.0 ==="]
FATAL_MARKERS = ["PANIC", "EXCEPTION", "FAILED:", "triple fault"]


def find_qemu():
    candidates = [
        os.environ.get("QEMU"),
        shutil.which("qemu-system-x86_64"),
        r"C:\Program Files\qemu\qemu-system-x86_64.exe",
        r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    ]
    for path in candidates:
        if not path:
            continue
        if os.path.isfile(path):
            return os.path.abspath(path)
    raise SystemExit("qemu-system-x86_64 not found; install QEMU or set QEMU=...")


def free_port():
    """Reserve an ephemeral loopback port for QEMU's serial server."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="seconds before the boot is declared hung")
    parser.add_argument("--expect", action="append", default=[],
                        help="additional serial marker that must appear")
    parser.add_argument("--expect-not", action="append", default=[],
                        metavar="TEXT",
                        help="text that must not appear anywhere in the serial "
                             "output (repeatable)")
    parser.add_argument("--input", action="append", default=[],
                        help="line sent to the guest once the shell prompt "
                             "appears (repeatable)")
    parser.add_argument("--settle", type=float, default=0.0,
                        help="seconds to keep the guest running after all "
                             "markers have appeared")
    args = parser.parse_args()

    if not os.path.isfile(OS_ELF):
        raise SystemExit("os.elf not found; run build.bat first")

    qemu = find_qemu()
    markers = DEFAULT_MARKERS + args.expect
    port = free_port()
    cmd = [
        qemu,
        "-kernel", OS_ELF,
        "-m", "512M",
        "-display", "none",
        "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % port,
        "-monitor", "none",
        "-no-reboot",
        "-no-shutdown",
        "-accel", "tcg",
    ]
    print("+ " + " ".join(cmd), flush=True)

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    qemu_log = []

    def pump_qemu():
        while True:
            chunk = proc.stdout.read1(4096)
            if not chunk:
                break
            qemu_log.append(chunk)

    threading.Thread(target=pump_qemu, daemon=True).start()

    # `wait=on` holds the VM until this connection arrives, so no boot output
    # is emitted before the reader is attached.
    sock = None
    connect_deadline = time.time() + 20.0
    while time.time() < connect_deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            break
        except OSError:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    if sock is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.stderr.write("BOOT FAILED: could not connect to QEMU serial port\n")
        log = b"".join(qemu_log).decode("utf-8", "replace")
        if log:
            sys.stderr.write(log)
        return 1

    chunks = []

    def pump_serial():
        while True:
            try:
                data = sock.recv(4096)
            except OSError:
                break
            if not data:
                break
            chunks.append(data)

    reader = threading.Thread(target=pump_serial, daemon=True)
    reader.start()

    def output():
        return b"".join(chunks).decode("utf-8", "replace")

    deadline = time.time() + args.timeout
    text = ""
    pending = list(args.input)
    next_input_at = 0.0
    while time.time() < deadline:
        text = output()
        fatal = next((m for m in FATAL_MARKERS if m in text), None)
        if fatal:
            break

        # One line per prompt: TCP backpressure handles the burst, but the
        # shell only reads one command at a time.
        if pending and "arc> " in text and time.time() >= next_input_at:
            line = pending.pop(0)
            try:
                sock.sendall((line + "\n").encode())
            except OSError:
                pending.clear()
            next_input_at = time.time() + 0.2

        if all(m in text for m in markers):
            if args.settle:
                time.sleep(args.settle)
            break
        if proc.poll() is not None:
            break
        time.sleep(0.05)

    time.sleep(0.2)
    text = output()
    fatal = next((m for m in FATAL_MARKERS if m in text), None)

    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    reader.join(timeout=2)
    text = output()
    fatal = next((m for m in FATAL_MARKERS if m in text), None)

    print("--- serial console ---")
    print(text, end="" if text.endswith("\n") else "\n")
    print("--- end serial console ---")

    missing = [m for m in markers if m not in text]
    forbidden = [m for m in args.expect_not if m in text]
    if fatal:
        print("BOOT FAILED: fatal marker %r" % fatal, file=sys.stderr)
        return 1
    if forbidden:
        print("BOOT FAILED: forbidden marker(s) present: %s"
              % ", ".join(map(repr, forbidden)), file=sys.stderr)
        return 1
    if missing:
        print("BOOT FAILED: missing markers: %s"
              % ", ".join(map(repr, missing)), file=sys.stderr)
        return 1
    print("BOOT OK: %d bytes of serial output" % len(text))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
