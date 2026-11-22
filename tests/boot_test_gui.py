#!/usr/bin/env python3
"""ARC OS M1 GUI QEMU gate: screendump pixel assertions."""
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

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


def parse_ppm(data: bytes):
    i = 0
    lines = []
    while len(lines) < 3:
        j = data.find(b"\n", i)
        if j < 0:
            raise ValueError("bad ppm")
        line = data[i:j]
        i = j + 1
        if line.startswith(b"#"):
            continue
        lines.append(line)
    if lines[0].strip() != b"P6":
        raise ValueError("expected P6 ppm")
    w, h = map(int, lines[1].split())
    maxv = int(lines[2].strip())
    if maxv != 255:
        raise ValueError("expected 8-bit ppm")
    pix = data[i:i + w * h * 3]
    if len(pix) < w * h * 3:
        raise ValueError("short ppm")
    return w, h, pix


def px(pix, w, x, y):
    off = (y * w + x) * 3
    return tuple(pix[off:off + 3])


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found; run build.bat first", file=sys.stderr)
        return 1
    qemu = find_qemu()
    sp, mp = free_port(), free_port()
    ppm_path = os.path.join(os.environ.get("TEMP", ROOT), "arc_m1_gui.ppm")
    cmd = [qemu, "-kernel", OS_ELF, "-m", "512M", "-display", "none",
           "-vga", "none", "-device", "bochs-display,vgamem=67108864",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % sp,
           "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % mp,
           "-no-reboot", "-no-shutdown", "-accel", "tcg"]
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    qlog = []

    def pump_qemu():
        while True:
            c = proc.stdout.read1(4096)
            if not c:
                break
            qlog.append(c)
    threading.Thread(target=pump_qemu, daemon=True).start()

    sock = None
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", sp), timeout=0.5)
            sock.settimeout(None)
            break
        except OSError:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    if sock is None:
        proc.terminate()
        print("BOOT FAILED: no serial", file=sys.stderr)
        return 1

    chunks = []

    def pump_serial():
        while True:
            try:
                d = sock.recv(4096)
            except OSError:
                break
            if not d:
                break
            chunks.append(d)
    threading.Thread(target=pump_serial, daemon=True).start()

    text = ""
    deadline = time.time() + 25
    markers = ["=== ARC OS v1.0 ===", "GFX: 1920x1080x32", "GFX: frame redraw cycles="]
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if any(m in text for m in ["PANIC", "EXCEPTION", "FAILED:"]):
            break
        if all(m in text for m in markers):
            time.sleep(0.5)
            break
        time.sleep(0.05)

    text = b"".join(chunks).decode("utf-8", "replace")
    print("--- serial console ---")
    print(text, end="" if text.endswith("\n") else "\n")
    print("--- end serial console ---")

    missing = [m for m in markers if m not in text]
    fatal = next((m for m in ["PANIC", "EXCEPTION", "FAILED:"] if m in text), None)
    if fatal or missing:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        sock.close()
        if fatal:
            print("GUI GATE FAILED: fatal marker %r" % fatal, file=sys.stderr)
        else:
            print("GUI GATE FAILED: missing %s" % missing, file=sys.stderr)
        return 1

    # screendump via HMP monitor
    try:
        mon = socket.create_connection(("127.0.0.1", mp), timeout=5)
        mon.settimeout(0.5)
        buf = b""
        end = time.time() + 5
        while time.time() < end:
            try:
                buf += mon.recv(4096)
            except Exception:
                pass
            if b"(qemu)" in buf:
                break
            time.sleep(0.05)
        mon.sendall(("screendump %s\n" % ppm_path.replace("\\", "/")).encode())
        end = time.time() + 10
        while time.time() < end:
            try:
                buf += mon.recv(4096)
            except Exception:
                pass
            if buf.count(b"(qemu)") >= 2:
                break
            time.sleep(0.1)
        mon.close()
    except Exception as e:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        sock.close()
        print("GUI GATE FAILED: monitor/screendump: %s" % e, file=sys.stderr)
        return 1

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    sock.close()

    if not os.path.isfile(ppm_path):
        print("GUI GATE FAILED: screendump not created: %s" % ppm_path, file=sys.stderr)
        return 1
    with open(ppm_path, "rb") as f:
        data = f.read()
    w, h, pix = parse_ppm(data)
    print("screendump: %dx%d" % (w, h))

    checks = [
        ("top-left wallpaper", 0, 0, (20, 40, 90)),
        ("top-right wallpaper", 1919, 0, (20, 40, 90)),
        ("bottom-left wallpaper", 0, 1079, (39, 79, 139)),
        ("bottom-right wallpaper", 1919, 1079, (39, 79, 139)),
        ("red test rect", 200, 150, (200, 40, 40)),
        ("text glyph pixel", 107, 257, (255, 255, 255)),
    ]
    failed = 0
    for name, x, y, want in checks:
        got = px(pix, w, x, y)
        ok = got == want
        print(("  PASS" if ok else "  FAIL") + " %s (%d,%d) got=%s want=%s" % (name, x, y, got, want))
        if not ok:
            failed += 1
    if w != 1920 or h != 1080:
        print("  FAIL resolution %dx%d want 1920x1080" % (w, h))
        failed += 1
    if failed:
        print("GUI GATE FAILED: %d checks failed" % failed, file=sys.stderr)
        return 1
    print("GUI GATE PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
