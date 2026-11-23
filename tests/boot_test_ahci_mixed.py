#!/usr/bin/env python3
"""M4.4 E mixed gate: ATA + AHCI FAT32 disks, AHCI chosen as root, ATA unchanged."""
import hashlib, os, shutil, socket, struct, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")
ATA_DISK = os.path.join(os.environ.get("TEMP", "/tmp"), "arc_m44_mixed_ata.img")
AHCI_DISK = os.path.join(os.environ.get("TEMP", "/tmp"), "arc_m44_mixed_ahci.img")


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


def make_fat32_image(path, total_sectors=32768, spc=1, reserved=32, num_fats=2, label=b'ARCOS      '):
    img = bytearray(total_sectors * 512)
    img[446 + 4] = 0x0C
    struct.pack_into("<I", img, 446 + 8, 2048)
    vol_sectors = total_sectors - 2048
    struct.pack_into("<I", img, 446 + 12, vol_sectors)
    img[510] = 0x55; img[511] = 0xAA
    b = 2048 * 512
    img[b:b+3] = b'\xEB\x58\x90'
    img[b+3:b+11] = b'MSDOS5.0'
    struct.pack_into("<H", img, b+11, 512)
    img[b+13] = spc
    struct.pack_into("<H", img, b+14, reserved)
    img[b+16] = num_fats
    struct.pack_into("<I", img, b+32, vol_sectors)
    spf = 1
    while True:
        data = vol_sectors - reserved - num_fats * spf
        clusters = data // spc
        need = (clusters + 2) * 4
        need_spf = (need + 511) // 512
        if need_spf == spf: break
        spf = need_spf
    struct.pack_into("<I", img, b+36, spf)
    struct.pack_into("<I", img, b+44, 2)
    struct.pack_into("<H", img, b+48, 1)
    struct.pack_into("<H", img, b+50, 6)
    img[b+64] = 0x80; img[b+66] = 0x29
    struct.pack_into("<I", img, b+67, 0x12345678)
    lab = (label + b'           ')[:11]
    img[b+71:b+82] = lab
    img[b+82:b+90] = b'FAT32   '
    img[b+510] = 0x55; img[b+511] = 0xAA
    fs = b + 512
    struct.pack_into("<I", img, fs+0, 0x41615252)
    struct.pack_into("<I", img, fs+484, 0x61417272)
    struct.pack_into("<I", img, fs+488, 0xFFFFFFFF)
    struct.pack_into("<I", img, fs+492, 0xFFFFFFFF)
    struct.pack_into("<I", img, fs+508, 0xAA550000)
    fat1 = b + reserved * 512
    struct.pack_into("<I", img, fat1+0, 0x0FFFFFF8)
    struct.pack_into("<I", img, fat1+4, 0x0FFFFFFF)
    struct.pack_into("<I", img, fat1+8, 0x0FFFFFF8)
    fat2 = fat1 + spf * 512
    img[fat2:fat2+12] = img[fat1:fat1+12]
    with open(path, 'wb') as f:
        f.write(img)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(65536), b''): h.update(c)
    return h.hexdigest()


def run_qemu(qemu, timeout=60):
    port = free_port()
    cmd = [qemu, "-kernel", OS_ELF, "-m", "512M", "-display", "none", "-vga", "none",
           "-device", "bochs-display,vgamem=67108864",
           "-drive", "file=%s,format=raw,if=ide,index=0" % ATA_DISK,
           "-device", "ich9-ahci,id=ahci",
           "-drive", "file=%s,format=raw,if=none,id=d1" % AHCI_DISK,
           "-device", "ide-hd,drive=d1,bus=ahci.0",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % port,
           "-monitor", "none", "-no-reboot", "-no-shutdown", "-accel", "tcg"]
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    chunks = []
    def pump_q():
        while True:
            c = proc.stdout.read1(4096)
            if not c: break
    threading.Thread(target=pump_q, daemon=True).start()
    sock = None
    dead = time.time() + 20
    while time.time() < dead:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            sock.settimeout(None); break
        except OSError:
            if proc.poll() is not None: break
            time.sleep(0.05)
    if sock is None:
        proc.terminate(); return ""
    def pump_s():
        while True:
            try: d = sock.recv(4096)
            except OSError: break
            if not d: break
            chunks.append(d)
    threading.Thread(target=pump_s, daemon=True).start()
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if "arc> " in text: time.sleep(0.5); break
        if "PANIC" in text or "EXCEPTION" in text: break
        time.sleep(0.05)
    try: sock.shutdown(socket.SHUT_RDWR)
    except OSError: pass
    sock.close()
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
    return b"".join(chunks).decode("utf-8", "replace")


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found", file=sys.stderr); return 1
    qemu = find_qemu()
    # E.1: ATA is plain QEMU disk (no ARCROOT), AHCI has ARCROOT label.
    # ARCROOT-qualified AHCI must be selected as root; ATA unchanged.
    make_fat32_image(ATA_DISK, label=b'NOARCROOT  ')
    make_fat32_image(AHCI_DISK, label=b'ARCROOT    ')
    ata_before = sha256(ATA_DISK)
    text = run_qemu(qemu)
    print("--- serial ---"); print(text); print("--- end serial ---")
    if "PANIC" in text or "EXCEPTION" in text:
        print("AHCI MIXED GATE FAILED: crash", file=sys.stderr); return 1
    if "STORAGE: root = ahci0" not in text:
        print("AHCI MIXED GATE FAILED: STORAGE: root = ahci0 not found", file=sys.stderr); return 1
    if "STORAGE: root = ata0" in text:
        print("AHCI MIXED GATE FAILED: ata0 chosen as root", file=sys.stderr); return 1
    ata_after = sha256(ATA_DISK)
    if ata_before != ata_after:
        print("AHCI MIXED GATE FAILED: ATA disk changed", file=sys.stderr); return 1
    if "arc> " not in text:
        print("AHCI MIXED GATE FAILED: no shell prompt", file=sys.stderr); return 1
    print("AHCI MIXED GATE PASSED")
    return 0


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
