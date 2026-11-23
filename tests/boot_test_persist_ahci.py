#!/usr/bin/env python3
"""M4.4 E: persistence on AHCI-only disk. Boot 1 write, boot 2 read back."""
import os, shutil, socket, struct, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OS_ELF = os.path.join(ROOT, "os.elf")
DISK = os.path.join(os.environ.get("TEMP", "/tmp"), "arc_m44_persist_ahci.img")


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


def make_fat32_image(path, total_sectors=32768, spc=1, reserved=32, num_fats=2):
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
    struct.pack_into("<H", img, b+17, 0)
    struct.pack_into("<H", img, b+19, 0)
    img[b+21] = 0xF8
    struct.pack_into("<H", img, b+22, 0)
    struct.pack_into("<H", img, b+24, 63)
    struct.pack_into("<H", img, b+26, 255)
    struct.pack_into("<I", img, b+28, 2048)
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
    struct.pack_into("<H", img, b+40, 0)
    struct.pack_into("<H", img, b+42, 0)
    struct.pack_into("<I", img, b+44, 2)
    struct.pack_into("<H", img, b+48, 1)
    struct.pack_into("<H", img, b+50, 6)
    img[b+64] = 0x80
    img[b+66] = 0x29
    struct.pack_into("<I", img, b+67, 0x12345678)
    img[b+71:b+82] = b'ARCOS      '
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


def run_qemu(qemu, inputs, timeout=60):
    port = free_port()
    cmd = [qemu, "-kernel", OS_ELF, "-m", "512M", "-display", "none", "-vga", "none",
           "-device", "bochs-display,vgamem=67108864",
           "-device", "ich9-ahci,id=ahci",
           "-drive", "file=%s,format=raw,if=none,id=d0" % DISK,
           "-device", "ide-hd,drive=d0,bus=ahci.0",
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
            sock.settimeout(None); break
        except OSError:
            if proc.poll() is not None: break
            time.sleep(0.05)
    if sock is None:
        proc.terminate(); return "", 1

    chunks = []
    def pump_s():
        while True:
            try: d = sock.recv(4096)
            except OSError: break
            if not d: break
            chunks.append(d)
    threading.Thread(target=pump_s, daemon=True).start()

    deadline = time.time() + timeout
    sent = 0
    prompt_seen = False
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if "arc> " in text or "arc>" in text:
            prompt_seen = True
        if prompt_seen and sent < len(inputs):
            time.sleep(0.3)
            line = inputs[sent]
            print(">> " + line, flush=True)
            try: sock.sendall((line + "\n").encode())
            except OSError: break
            sent += 1
            prompt_seen = False
            time.sleep(0.2)
        if sent >= len(inputs) and prompt_seen:
            time.sleep(0.5)
            break
        if "PANIC" in text or "EXCEPTION" in text:
            break
        time.sleep(0.05)

    try: sock.shutdown(socket.SHUT_RDWR)
    except OSError: pass
    sock.close()
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
    time.sleep(0.2)
    return b"".join(chunks).decode("utf-8", "replace"), 0


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found", file=sys.stderr); return 1
    qemu = find_qemu()
    make_fat32_image(DISK)

    text1, _ = run_qemu(qemu, ["echo hello > persist.txt", "sync", "cat persist.txt"], timeout=60)
    print("--- boot 1 ---"); print(text1); print("--- end boot 1 ---")
    if "PANIC" in text1 or "EXCEPTION" in text1:
        print("PERSIST_AHCI GATE FAILED: crash in boot 1", file=sys.stderr); return 1
    if "STORAGE: root = ahci0" not in text1:
        print("PERSIST_AHCI GATE FAILED: STORAGE: root = ahci0 not found", file=sys.stderr); return 1
    if "ATA" in text1 and "ata0" in text1.lower() and "mounted" in text1.lower():
        # be careful not to false-positive; check for ATA mount specifically
        pass
    for line in text1.splitlines():
        if "STORAGE: root = ata" in line or "ATA PIO fallback" in line and "mounted" in line:
            print("PERSIST_AHCI GATE FAILED: ATA mount detected: " + line, file=sys.stderr); return 1
    if "sync: 0" not in text1:
        print("PERSIST_AHCI GATE FAILED: sync: 0 not found", file=sys.stderr); return 1

    text2, _ = run_qemu(qemu, ["cat persist.txt"], timeout=60)
    print("--- boot 2 ---"); print(text2); print("--- end boot 2 ---")
    if "PANIC" in text2 or "EXCEPTION" in text2:
        print("PERSIST_AHCI GATE FAILED: crash in boot 2", file=sys.stderr); return 1
    if "STORAGE: root = ahci0" not in text2:
        print("PERSIST_AHCI GATE FAILED: STORAGE: root = ahci0 not found in boot 2", file=sys.stderr); return 1
    after_cat = text2.split("cat persist.txt")[-1] if "cat persist.txt" in text2 else text2
    if "hello" not in after_cat:
        print("PERSIST_AHCI GATE FAILED: content not found after reboot", file=sys.stderr); return 1

    print("PERSIST_AHCI GATE PASSED")
    return 0


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
