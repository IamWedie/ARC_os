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
                ok = False
                print("  FAIL %s (%d,%d) got=%s" % (name, x, y, got))
            else:
                ok = near(got, want, 25)
                print(("  PASS" if ok else "  FAIL") + " %s (%d,%d) got=%s want~%s" % (name, x, y, got, want))
            if not ok:
                failed += 1
        if w != 1920 or h != 1080:
            print("  FAIL resolution %dx%d" % (w, h))
            failed += 1

        # M2 interaction checks: mouse move/click/window appear/drag/close/dock hover
        mouse = [960, 540]
        def accel(v):
            av = abs(v)
            if av <= 2: return v
            if av <= 20: return v * 2
            return v * 3

        def find_cursor():
            data = screendump(mon, ppm_path)
            ww, hh, pp = parse_ppm(data)
            def P(x, y):
                if x < 0 or y < 0 or x >= ww or y >= hh: return (-1, -1, -1)
                o = (y * ww + x) * 3
                return tuple(pp[o:o+3])
            for y in range(hh - 12):
                for x in range(ww - 8):
                    if (P(x, y) == (0, 0, 0) and P(x, y+1) == (0, 0, 0) and
                        P(x+1, y+1) == (0, 0, 0) and P(x, y+2) == (0, 0, 0)):
                        # verify looks like cursor: (x+1,y) should be non-black
                        if P(x+1, y) != (0, 0, 0):
                            return x, y
            return None

        def move_to(tx, ty):
            for _ in range(300):
                cur = find_cursor()
                if cur:
                    mouse[0], mouse[1] = cur
                if abs(mouse[0] - tx) <= 2 and abs(mouse[1] - ty) <= 2:
                    return
                sdx = tx - mouse[0]
                sdy = ty - mouse[1]
                def pick(sd):
                    ad = abs(sd)
                    sign = 1 if sd > 0 else -1
                    if ad <= 2: return sd
                    if ad <= 40: return sign * max(1, min(20, ad // 2))
                    return sign * max(1, min(60, ad // 3))
                rx = pick(sdx)
                ry = pick(sdy)
                hmp_cmd(mon, "mouse_move %d %d" % (rx, ry), wait=0.05)
                mouse[0] = max(0, min(1919, mouse[0] + accel(rx)))
                mouse[1] = max(0, min(1079, mouse[1] + accel(ry)))
            time.sleep(0.2)

        def click_at(tx, ty):
            for ox, oy in [(0,0),(2,0),(-2,0),(0,2),(0,-2)]:
                move_to(tx+ox, ty+oy)
                hmp_cmd(mon, "mouse_button 1", wait=0.08)
                hmp_cmd(mon, "mouse_button 0", wait=0.12)
            time.sleep(0.3)

        def dump_check(name, x, y, pred, desc):
            nonlocal failed, pix, w, h
            data = screendump(mon, ppm_path)
            w, h, pix = parse_ppm(data)
            got = px(pix, w, x, y)
            ok = pred(got)
            print(("  PASS" if ok else "  FAIL") + " %s (%d,%d) got=%s %s" % (name, x, y, got, desc))
            if not ok:
                failed += 1

        # Open Notes via dock icon (index 1)
        notes_x = 1920//2 - (4*56 + 3*8)//2 + 1*(56+8) + 28
        notes_y = 1080 - 40
        click_at(notes_x, notes_y)
        time.sleep(1.5)
        dump_check("notes window appeared", 250, 210, lambda c: c[0] > 220 and c[1] > 220, "expect bright window")

        # Drag About window by title bar (About at 120,80)
        for sy in [94, 92, 96]:
            move_to(300, sy)
            time.sleep(0.2)
            hmp_cmd(mon, "mouse_button 1", wait=0.12)
            move_to(500, 150)
            hmp_cmd(mon, "mouse_button 0", wait=0.2)
            time.sleep(0.8)
        time.sleep(1.0)
        data = screendump(mon, ppm_path)
        w, h, pix = parse_ppm(data)
        got_new = px(pix, w, 600, 200)
        got_old = px(pix, w, 130, 150)
        drag_ok = got_new[0] > 220 and got_new[1] > 220
        old_ok = got_old[0] < 120
        print(("  PASS" if drag_ok else "  FAIL") + " drag moved window (600,200) got=%s expect window at new pos" % (got_new,))
        print(("  PASS" if old_ok else "  FAIL") + " drag left old area (130,150) got=%s expect wallpaper" % (got_old,))
        if not drag_ok: failed += 1
        if not old_ok: failed += 1

        # Close Notes window (opened at 150,104, still there)
        for _ in range(3):
            click_at(168, 118)
            time.sleep(1.0)
        time.sleep(1.5)
        dump_check("close removed window", 250, 210, lambda c: c[0] < 150, "expect wallpaper/shadow gone")

        # Dock hover over About icon
        about_x = 1920//2 - (4*56 + 3*8)//2 + 0*(56+8) + 28
        move_to(about_x, 1080 - 40)
        time.sleep(0.5)
        dump_check("dock hover", about_x - 10, 1080 - 45, lambda c: c != (0,0,0) and (c[0] > 40 or c[1] > 80 or c[2] > 120), "expect icon/dock pixel")

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
