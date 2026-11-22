#!/usr/bin/env python3
"""ARC OS M2 GUI QEMU gate: desktop screendump assertions."""
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


def near(got, want, tol=20):
    return all(abs(got[i] - want[i]) <= tol for i in range(3))


def hmp_cmd(mon, cmd, wait=0.3):
    mon.sendall((cmd + "\n").encode())
    time.sleep(wait)
    buf = b""
    mon.settimeout(0.2)
    try:
        while True:
            buf += mon.recv(4096)
    except Exception:
        pass
    return buf


def screendump(mon, path):
    hmp_cmd(mon, "screendump %s" % path.replace("\\", "/"), wait=1.0)
    with open(path, "rb") as f:
        return f.read()


def main():
    if not os.path.isfile(OS_ELF):
        print("os.elf not found; run build.bat first", file=sys.stderr)
        return 1
    qemu = find_qemu()
    sp, mp = free_port(), free_port()
    ppm_path = os.path.join(os.environ.get("TEMP", ROOT), "arc_m2_gui.ppm")
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

    markers = ["=== ARC OS v1.0 ===", "GFX: 1920x1080x32",
               "PS/2 mouse initialized", "GUI: desktop initialized"]
    text = ""
    deadline = time.time() + 30
    while time.time() < deadline:
        text = b"".join(chunks).decode("utf-8", "replace")
        if any(m in text for m in ["PANIC", "EXCEPTION", "FAILED:"]):
            break
        if all(m in text for m in markers):
            time.sleep(4.0)  # let GUI render a frame under TCG
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

    try:
        mon = socket.create_connection(("127.0.0.1", mp), timeout=5)
        mon.settimeout(0.5)
        # drain banner
        end = time.time() + 5
        buf = b""
        while time.time() < end:
            try:
                buf += mon.recv(4096)
            except Exception:
                pass
            if b"(qemu)" in buf:
                break
            time.sleep(0.05)

        data = screendump(mon, ppm_path)
        w, h, pix = parse_ppm(data)
        print("screendump: %dx%d" % (w, h))
        # retry if GUI hasn't rendered yet (still clear color)
        if near(px(pix, w, 500, 14), (20, 40, 90), 5):
            time.sleep(4.0)
            data = screendump(mon, ppm_path)
            w, h, pix = parse_ppm(data)
            print("screendump(retry): %dx%d" % (w, h))

        checks = []
        # wallpaper visible in corners (gradient)
        checks.append(("top-left wallpaper", 0, 100, (21, 43, 94)))
        checks.append(("bottom-right wallpaper", 1919, 1079, (39, 79, 139)))
        # menu bar area - translucent white over wallpaper, should be bright
        mb = px(pix, w, 500, 14)
        checks.append(("menu bar bright", 500, 14, mb if mb[0] > 150 and mb[1] > 150 and mb[2] > 160 else (-1, -1, -1)))
        # dock area near bottom center
        dock = px(pix, w, 960, 1040)
        checks.append(("dock visible", 960, 1040, dock if dock[0] > 120 else (-1, -1, -1)))
        # About window should be visible around (120,80)-(540,360)
        win_title = px(pix, w, 200, 95)
        checks.append(("window title bar", 200, 95, win_title if win_title[0] > 200 else (-1, -1, -1)))
        win_body = px(pix, w, 200, 150)
        checks.append(("window body", 200, 150, win_body if win_body[0] > 220 and win_body[1] > 220 else (-1, -1, -1)))

        failed = 0
        for name, x, y, want in checks:
            got = px(pix, w, x, y)
            if want == (-1, -1, -1):
                ok = False  # sentinel means condition failed above
                print("  FAIL %s (%d,%d) got=%s" % (name, x, y, got))
            else:
                ok = near(got, want, 25)
                print(("  PASS" if ok else "  FAIL") + " %s (%d,%d) got=%s want~%s" % (name, x, y, got, want))
            if not ok:
                failed += 1
        if w != 1920 or h != 1080:
            print("  FAIL resolution %dx%d" % (w, h))
            failed += 1

        mon.close()
    except Exception as e:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        sock.close()
        print("GUI GATE FAILED: %s" % e, file=sys.stderr)
        return 1

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    sock.close()

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
