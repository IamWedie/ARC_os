#!/usr/bin/env python3
"""ARC OS M3 gate: Multiboot2 header presence + boot path smoke test.

QEMU 11.x -kernel on this environment uses Multiboot1 when both headers are
present; the kernel accepts both protocols and reports which one was used.
This gate verifies the MB2 header is present/valid in os.elf and that the
kernel boots with the normalized boot-info path.
"""
import os, shutil, socket, struct, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")


def find_qemu():
    for p in [os.environ.get("QEMU"), shutil.which("qemu-system-x86_64"),
              r"C:\Program Files\qemu\qemu-system-x86_64.exe"]:
        if p and os.path.isfile(p):
            return os.path.abspath(p)
    raise SystemExit("qemu-system-x86_64 not found")


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def verify_mb2_header(path):
    d = open(path, "rb").read(32768)
    magic = b"\xd6\x50\x52\xe8"
    off = 0
    while off + 16 <= len(d):
        if d[off:off+4] == magic:
            m, arch, hlen, csum = struct.unpack_from("<IIII", d, off)
            if (m + arch + hlen + csum) & 0xFFFFFFFF == 0 and arch == 0:
                print("MB2 header OK at file offset %d (len=%d)" % (off, hlen))
                return True
        off += 8
    print("MB2 header NOT FOUND/INVALID", file=sys.stderr)
    return False


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found", file=sys.stderr)
        return 1
    if not verify_mb2_header(OS_ELF):
        return 1

    qemu = find_qemu()
    port = free_port()
    cmd = [qemu, "-kernel", OS_ELF, "-m", "512M", "-display", "none", "-vga", "none",
           "-device", "bochs-display,vgamem=67108864",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % port,
           "-monitor", "none", "-no-reboot", "-no-shutdown", "-accel", "tcg"]
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    qlog = []
    def pump_q():
        while True:
            c = proc.stdout.read1(4096)
            if not c: break
            qlog.append(c)
    threading.Thread(target=pump_q, daemon=True).start()

    sock = None
    dead = time.time() + 20
    while time.time() < dead:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            sock.settimeout(None)
            break
        except OSError:
            if proc.poll() is not None: break
            time.sleep(0.05)
    if sock is None:
        proc.terminate()
        print("BOOT FAILED: no serial", file=sys.stderr)
        return 1

    chunks = []
    def pump_s():
        while True:
            try: d = sock.recv(4096)
            except OSError: break
            if not d: break
            chunks.append(d)
    threading.Thread(target=pump_s, daemon=True).start()

    markers_any = ["BOOT: Multiboot1", "BOOT: Multiboot2"]
    markers_all = ["=== ARC OS v1.0 ===", "1920x1080x32", "GUI: desktop initialized"]
    fatal = ["PANIC", "EXCEPTION", "FAILED:"]
    deadline = time.time() + 30
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if any(m in text for m in fatal): break
        if any(m in text for m in markers_any) and all(m in text for m in markers_all):
            time.sleep(0.5); break
        time.sleep(0.05)

    try: sock.shutdown(socket.SHUT_RDWR)
    except OSError: pass
    sock.close()
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
    time.sleep(0.2)
    text = b"".join(chunks).decode("utf-8", "replace")
    print("--- serial console ---")
    print(text, end="" if text.endswith("\n") else "\n")
    print("--- end serial console ---")

    fat = next((m for m in fatal if m in text), None)
    if fat:
        print("MB2 GATE FAILED: fatal %r" % fat, file=sys.stderr); return 1
    if not any(m in text for m in markers_any):
        print("MB2 GATE FAILED: missing boot protocol marker", file=sys.stderr); return 1
    missing = [m for m in markers_all if m not in text]
    if missing:
        print("MB2 GATE FAILED: missing %s" % missing, file=sys.stderr); return 1
    proto = "Multiboot2" if "BOOT: Multiboot2" in text else "Multiboot1"
    print("MB2 GATE PASSED (boot protocol: %s)" % proto)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)