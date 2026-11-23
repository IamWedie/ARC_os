#!/usr/bin/env python3
"""M4.4 AHCI gate: boot with ich9-ahci, verify identify + selftest, data disk unchanged."""
import hashlib, os, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")
DISK = os.path.join(os.environ.get("TEMP", "/tmp"), "arc_m4_ahci.img")
SCRATCH = os.path.join(os.environ.get("TEMP", "/tmp"), "arc_m4_ahci_scratch.img")


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


def ensure_disk(path):
    if os.path.isfile(path) and os.path.getsize(path) >= 16 * 1024 * 1024:
        return
    with open(path, "wb") as f:
        f.truncate(16 * 1024 * 1024)

def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def run_qemu(qemu, timeout=45):
    port = free_port()
    ensure_disk(DISK)
    # Fresh scratch disk for selftest; must be zeroed.
    with open(SCRATCH, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    cmd = [
        qemu, "-kernel", OS_ELF, "-m", "512M",
        "-display", "none", "-vga", "none",
        "-device", "bochs-display,vgamem=67108864",
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,format=raw,if=none,id=ahcidisk" % DISK,
        "-device", "ide-hd,drive=ahcidisk,bus=ahci.0",
        "-drive", "file=%s,format=raw,if=none,id=scratch" % SCRATCH,
        "-device", "ide-hd,drive=scratch,bus=ahci.1,serial=ARCSCRATCH",
        "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % port,
        "-monitor", "none", "-no-reboot", "-no-shutdown", "-accel", "tcg",
    ]
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    qlog = []

    def pump_q():
        while True:
            c = proc.stdout.read1(4096)
            if not c:
                break
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
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    if sock is None:
        proc.terminate()
        return "", 1

    chunks = []

    def pump_s():
        while True:
            try:
                d = sock.recv(4096)
            except OSError:
                break
            if not d:
                break
            chunks.append(d)
    threading.Thread(target=pump_s, daemon=True).start()

    deadline = time.time() + timeout
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if "arc> " in text:
            time.sleep(0.5)
            break
        if "PANIC" in text or "EXCEPTION" in text:
            break
        time.sleep(0.05)

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
    time.sleep(0.2)
    text = b"".join(chunks).decode("utf-8", "replace")
    return text, 0


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found", file=sys.stderr)
        return 1
    qemu = find_qemu()
    ensure_disk(DISK)
    data_before = file_sha256(DISK)
    text, _ = run_qemu(qemu, timeout=90)
    print("--- serial ---")
    print(text)
    print("--- end serial ---")

    if "PANIC" in text or "EXCEPTION" in text:
        print("AHCI GATE FAILED: crash detected", file=sys.stderr)
        return 1

    # Required capability line: version, ports, S64A
    found = False
    for line in text.splitlines():
        if "AHCI:" in line and "version" in line and "ports=" in line and "s64a=" in line:
            found = True
            print("AHCI capability line: " + line.strip())
            break
    if not found:
        print("AHCI GATE FAILED: AHCI capability line (version, ports, S64A) not found", file=sys.stderr)
        return 1

    started = [ln.strip() for ln in text.splitlines()
               if "AHCI: port" in ln and "sig=" in ln and "started" in ln]
    if not started:
        print("AHCI GATE FAILED: no started SATA port line found", file=sys.stderr)
        return 1
    for ln in started:
        print("AHCI started port: " + ln)

    ident_lines = [ln.strip() for ln in text.splitlines()
                   if "AHCI: port" in ln and "sectors=" in ln and "ss=" in ln]
    if len(ident_lines) < 2:
        print("AHCI GATE FAILED: expected identify lines for both disks, got %d" % len(ident_lines), file=sys.stderr)
        return 1
    for il in ident_lines:
        print("AHCI identify line: " + il)
        if "ss=512" not in il:
            print("AHCI GATE FAILED: expected ss=512 in identify line", file=sys.stderr)
            return 1
        if "lba48=1" not in il and "lba48=0" not in il:
            print("AHCI GATE FAILED: missing lba48 field in identify line", file=sys.stderr)
            return 1
    disk_sectors = os.path.getsize(DISK) // 512
    if not any(("sectors=%d" % disk_sectors) in il for il in ident_lines):
        print("AHCI GATE FAILED: expected sectors=%d in an identify line" % disk_sectors, file=sys.stderr)
        return 1

    if "AHCI: selftest PASS" not in text:
        print("AHCI GATE FAILED: AHCI selftest PASS not found", file=sys.stderr)
        return 1
    print("AHCI selftest PASS found")

    data_after = file_sha256(DISK)
    if data_before != data_after:
        print("AHCI GATE FAILED: data disk changed during selftest", file=sys.stderr)
        return 1
    print("AHCI data disk unchanged")

    if "arc> " not in text:
        print("AHCI GATE FAILED: shell prompt not reached", file=sys.stderr)
        return 1

    print("AHCI GATE PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
