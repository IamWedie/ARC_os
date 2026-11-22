# Roadmap: ARC OS, from QEMU kernel to a real OS (24 months)

Status: DRAFT (awaiting approval)
Suggested location: `docs/specs/arc-os-24-month-roadmap.md`
Starting point: `main` at `b25aac8` (fork/wait + exec loader integrated)
Window: December 2026 to November 2028 (24 months)
Budget assumption: 10 to 15 hours per week, about 50 hours per month. Exam
periods count as half-capacity months.

## 1. Definition of "real OS" for this project

Target level (must ship):

- Boots from a USB stick or SSD on at least one real PC (not only QEMU).
- Shows a modern graphical desktop at 1920x1080 with mouse and keyboard.
- Keeps files on a real disk and survives reboot.
- Runs userspace programs loaded from disk (ELF), written against a documented
  syscall ABI and a small libc.
- Ships at least four real apps: terminal, text editor, file manager, system
  monitor.

Stretch level (ship if time allows, in this order):

1. USB keyboard and mouse (xHCI HID).
2. Networking: Ethernet, IPv4, ARP, ICMP, UDP, DHCP, DNS, then TCP.
3. Multicore (SMP) and SIMD-accelerated compositing.

Non-goals (do not start these, they cost months each):

- Running Windows or Linux applications, Win32 or POSIX completeness.
- A real GPU driver (Intel, AMD, NVIDIA). A fast CPU compositor is the target.
  virtio-gpu is optional and only for QEMU.
- Wi-Fi, Bluetooth, audio, suspend/resume, ACPI power management.
- A web browser. A tiny HTTP fetch tool is the networking demo.

## 2. Rules that apply to every milestone

1. Append-only ABI: never renumber syscalls 1 to 16. New syscalls start at 17.
2. A milestone is done only when ALL of these pass:
   - host tests (`tests\run_tests.bat`), append new suites, never edit old ones
   - full build (`python tools\build_kernel.py --quiet`)
   - `tests\run_all.bat` (frozen, never weakened)
   - the milestone's own QEMU gate (named in each section)
3. Relaxing a recorded decision needs a new ADR first. Example: ADR-004
   (no SSE) is superseded by a new ADR before any SIMD code lands.
4. One milestone, one branch, one review. Merge to `main` only when the done
   criteria are met. Tag each merge: `m1-done`, `m2-done`, and so on.
5. Real-hardware check: from M3 onward, every milestone ends with a boot on the
   real test machine. Record the result in `tasks/hardware-log.md`
   (machine, firmware mode, what worked, what failed).
6. Time box: if a milestone exceeds 150 percent of its estimate, stop and
   apply the cut list in section 6.

## 3. Calendar overview

| ID | Milestone | Calendar | Hours (est.) | Must or stretch |
|----|-----------|----------|--------------|-----------------|
| M0 | Baseline and housekeeping | Dec 2026 (weeks 1-2) | 15 | Must |
| M1 | 1080p framebuffer, font, double buffering | Dec 2026 to Jan 2027 | 70 | Must |
| M2 | Mouse, compositor, windows, dock, menu bar | Feb to Mar 2027 | 100 | Must |
| M3 | Real-hardware boot path (GRUB Multiboot2 and UEFI) | Apr to May 2027 | 80 | Must |
| M4 | Persistent storage: AHCI, FAT32, VFS | Jun to Aug 2027 | 150 | Must |
| M5 | Real userspace: ELF, syscall ABI, libc | Sep to Nov 2027 | 150 | Must |
| M6 | Userspace GUI: window syscalls, event queue, apps | Dec 2027 to Feb 2028 | 150 | Must |
| M7 | Desktop session, settings, app launcher | Mar 2028 | 50 | Must |
| M8 | USB (xHCI) keyboard and mouse | Apr to May 2028 | 100 | Stretch 1 |
| M9 | Networking | Jun to Aug 2028 | 150 | Stretch 2 |
| M10 | SMP and SIMD | Sep to Oct 2028 | 80 | Stretch 3 |
| M11 | Hardening and performance | Oct to Nov 2028 | 50 | Must |
| M12 | Release, docs, demo, report | Nov 2028 | 40 | Must |

Estimated total: about 1,185 hours against about 1,200 available. There is
almost no slack, which is why section 6 (cut list) exists. Treat M8 to M10 as
the buffer.

---

## M0: Baseline and housekeeping (weeks 1-2)

Goal: make the repo state match reality, so everything after is traceable.

Steps:

1. Push the work: `git push -u origin gui-desktop`.
2. Copy the GUI design from your notes to
   `docs/specs/gui-desktop-design.md` (use the same section style as
   `process-exec-loader-design.md`: Intent, Goals, Non-goals, Design, Verification).
3. Copy this roadmap to `docs/specs/arc-os-24-month-roadmap.md` and link it
   from `README.md`.
4. Create `tasks/hardware-log.md` and `tasks/risk-log.md` (empty tables).
5. Write ADR-007 "Display path": Bochs DISPI for QEMU dev, Multiboot2
   framebuffer tag for real hardware (M3). Status: Proposed.
6. Record the toolchain versions (Zig/Clang, QEMU, Python) in `README.md`.

Done when: `gui-desktop` is on GitHub, both docs are committed, the three logs
exist, `tests\run_all.bat` is green.

## M1: 1080p framebuffer, font, double buffering

Goal: a stable 1920x1080x32 screen with fast, flicker-free drawing.

Prerequisites: M0.

Steps:

1. Extend `gfx_init()` to read the max resolution from DISPI registers
   (index 7 and 8 are max xres/yres on Bochs VBE). Request 1920x1080x32; fall
   back to 1280x720, then 1024x768.
2. Change QEMU args in `run.bat` and every gate: `-vga none -device
   bochs-display,vgamem=67108864`. 1080p x 4 bytes is about 8 MiB per frame, so
   64 MiB fits front buffer, back buffer, and cached assets.
3. Add a RAM back buffer (`gfx_backbuf`) allocated from the PMM (use 2 MiB
   pages when possible). All drawing goes to the back buffer.
4. Add `gfx_present(rect)` that copies only the dirty rectangle to the
   framebuffer. Keep a dirty-rect list with merge logic (host-testable).
5. Replace the 8x8 bitmap font for UI text. Embed one OFL-licensed font
   (Inter or similar), pre-rasterized at 12, 14, 18, 24 px as 8-bit coverage
   bitmaps (generated by a script in `tools/`, output committed as C arrays).
   Render with alpha blending against any background.
6. Add `gfx_blend_pixel` and `gfx_fill_rect_alpha` using premultiplied alpha.
   Scalar code only in this milestone.
7. Add a timer-driven frame pacer (60 Hz target from the LAPIC timer). Skip
   frames when nothing is dirty.

Tests:

- Host: `tests/test_gfx.c` (suite 12): clip, fill, alpha blend math, dirty-rect
  merge, font coverage blend.
- QEMU gate: `tests/boot_test_gui.py` takes `screendump`, asserts exact pixel
  colors at known coordinates (wallpaper corners, a test rectangle, a text
  glyph pixel).

Done when: 1920x1080 desktop with a gradient wallpaper and anti-aliased text
renders; fallback resolutions work; frame time for a full redraw is logged.

Risks: VRAM size (fix: `vgamem`), slow per-pixel code (fix: row-wise fills and
dirty rects, not SIMD yet).

## M2: Mouse, compositor, windows, dock, menu bar (kernel-mode)

Goal: the macOS-style look and interaction, running inside the kernel.

Prerequisites: M1.

Steps:

1. PS/2 mouse driver (IRQ12, 3-byte packets, clamping, acceleration curve).
   Packet decode is a pure function so it can be host-tested.
2. Compositor core: window table (16 windows), Z-order, per-window ARGB surface
   in RAM, premultiplied-alpha blend in Z-order, damage tracking.
3. Visual layer: rounded corners (radius 10) via cached masks, cached 9-slice
   drop shadows, traffic-light buttons, active versus inactive title bar.
4. Menu bar (translucent, 28 px): logo, app name, clock. Blur-behind using a
   box blur on a downscaled copy, cached when the wallpaper does not change.
5. Dock: centered pill, icon magnification on hover, running indicator.
6. Animations: easing functions (ease-out, spring) driven by the frame pacer
   for open, close, and minimize.
7. Built-in apps (in kernel for now): About, Notes, Files, System Monitor
   (RAM used/free, FPS, uptime; per-core usage comes in M10).
8. Keyboard routing: focused window gets keys only if it has a text field;
   otherwise keys still reach the ring-3 shell so existing gates stay green.

Tests:

- Host: `tests/test_gui.c` (suite 13): hit-testing, Z-order, drag deltas,
  mask math, easing curves, mouse decode.
- QEMU gate: inject `mouse_move` and clicks via the monitor; assert window
  appears, drag changes pixel positions, close removes it, dock hover changes
  icon size.

Done when: a person can open, drag, focus, and close at least three windows
with smooth animation, with no flicker, at 1080p.

Risks: blur cost (fix: downscale, cache), mouse jitter (fix: clamp and
accumulate sub-pixel remainders).

## M3: Real-hardware boot path

Goal: the same kernel boots and shows the desktop on a physical PC.

Prerequisites: M2. A spare PC or laptop (any x86-64 from the last 12 years).
USB stick of at least 2 GB.

Why this milestone exists: your spike showed Multiboot1 VBE returns an empty
framebuffer, and the Bochs DISPI trick only exists in QEMU.

Steps:

1. Spike (1 week, throwaway): boot with GRUB and Multiboot2 requesting a
   framebuffer tag (type 8) in both QEMU and on the test PC. Confirm
   `framebuffer_addr`, `pitch`, `width`, `height`, `bpp` are non-zero. If yes,
   this is the path. If no, write a minimal UEFI loader that uses GOP and hands
   off a boot-info struct.
2. Add a Multiboot2 header and boot-info parser. Keep the Multiboot1 path
   working for existing gates until the new path is green.
3. Add a `display_backend` interface: `bochs_dispi` (QEMU) and `boot_fb`
   (firmware framebuffer). The compositor only sees the interface.
4. Write `tools/make_usb_image.py` to build a bootable image (GRUB + kernel).
   Document how to flash it (Rufus or `dd`).
5. Add early debug output that works without serial: on-screen log region and a
   panic screen showing registers and a stack trace.
6. Harden PMM and memory map parsing against real firmware maps (holes,
   reserved ranges, ACPI regions, memory above 4 GiB).
7. Boot on the real PC. Record in `tasks/hardware-log.md`. Fix what breaks.

Tests:

- Gate: `tests/boot_test_mb2.bat` (QEMU, new header path).
- Manual: real-PC checklist (section 8).

Done when: desktop appears on real hardware at the screen's native
resolution (or 1080p), the mouse works (PS/2 or legacy USB emulation), and the
result is logged.

Risks: firmware differences (UEFI-only machines, Secure Boot). Use a machine
with Secure Boot disabled and CSM/legacy support to start.

## M4: Persistent storage

Goal: files survive reboot.

Prerequisites: M3 (a real disk to test on), PCI enumeration.

Steps:

1. PCI enumeration module (`kernel/pci.c`): scan buses, record vendor, device,
   class, BARs, IRQ. Reuse it later for AHCI, xHCI, and NICs.
2. Block layer (`kernel/blockdev.c`): `read_sectors`, `write_sectors`,
   `flush`, a small block cache (write-through first, write-back later).
3. Keep the existing ATA PIO driver as the QEMU baseline; wrap it in the block
   interface.
4. AHCI driver (`kernel/ahci.c`): detect controller, set up command list and
   FIS area, issue READ/WRITE DMA EXT, interrupt-driven completion.
5. Partition tables: MBR and GPT parsing (read-only).
6. FAT32 first (read, then write, then long file names, then directories).
   Reason: you can inspect the disk image from Windows to debug.
7. VFS layer: mount table, path resolution, a `struct vfs_node` interface so
   RAM-FS and FAT32 plug in the same way. Keep your fd layer as is.
8. New syscalls (17+): `SYS_STAT`, `SYS_READDIR`, `SYS_MKDIR`, `SYS_UNLINK`,
   `SYS_MOUNT`. Document them in `docs/specs/syscall-abi.md`.
9. Boot from the data partition (kernel loads the first user app from disk, not
   from an embedded blob).
10. Crash-safety basics: ordered writes for metadata, `fsck`-like check tool in
    `tools/`.

Tests:

- Host: FAT32 parser and allocator against an in-memory image; fuzz the parser
  with random corrupt images.
- QEMU gate: create a file, reboot, read it back (`tests/boot_test_persist.bat`).
- Real PC: write a file, power off, boot again, file exists.

Done when: files and directories persist across reboot on QEMU and on the real
disk, and the Files app lists the real filesystem.

Risks: data loss on the real PC. Use a dedicated test disk or USB stick, never
your main disk. AHCI is easy to get 90 percent right and hard to finish; budget
extra time for error paths.

## M5: Real userspace

Goal: programs are separate files, built with a toolchain, and run in ring 3.

Prerequisites: M4.

Steps:

1. ELF64 loader replacing the flat-binary loader: parse program headers, map
   PT_LOAD segments with correct permissions (R, RW, RX), set up stack with
   argc/argv/envp/auxv. Keep the flat loader for existing blobs until all
   gates use ELF.
2. Memory syscalls: `SYS_BRK` and `SYS_MMAP`/`SYS_MUNMAP` (anonymous only),
   with VMA tracking per process.
3. Page-fault handler for demand paging of stack and heap. Add copy-on-write for
   fork only if time remains (otherwise keep the current deep-copy fork).
4. Syscall ABI document and a version number. Add `SYS_GETABI`.
5. Userspace toolchain: `tools/build_user.py` builds apps with a freestanding
   target, a linker script, and `crt0.S`.
6. Minimal libc (`userspace/libc`): string.h, ctype.h, stdlib (malloc/free over
   brk/mmap, qsort, atoi), stdio (printf family, FILE over fds), time basics.
7. Rewrite the shell as a normal ELF program on disk; keep the embedded one as
   a recovery shell.
8. Signals-lite: `SYS_KILL` with a few fixed signals; process groups not needed.
9. Environment: `argv`, `envp`, current directory per process.

Tests:

- Host: ELF parser (valid, truncated, overlapping segments, huge sizes).
- Syscall fuzz test: random arguments must never crash the kernel
  (`tests/fuzz_syscalls.py`).
- QEMU gate: run three ELF programs from disk and check their output and exit
  codes (`tests/boot_test_elf.bat`).

Done when: you can write a new C program, build it with one command, copy it
to the disk image, and run it from the shell with arguments.

Risks: the biggest silent risk is kernel security (bad user pointers). Add
`copy_from_user`/`copy_to_user` helpers and use them everywhere.

## M6: Userspace GUI

Goal: apps draw their own windows; the kernel only composites.

Prerequisites: M5.

Steps:

1. Syscalls (appended from the next free number): `SYS_WIN_CREATE`,
   `SYS_WIN_SURFACE` (map a shared surface), `SYS_WIN_PRESENT` (dirty rect),
   `SYS_WIN_SET_TITLE`, `SYS_WIN_CLOSE`, `SYS_EVENT_POLL`/`SYS_EVENT_WAIT`.
2. Shared surfaces: kernel allocates pages, maps them into the app's address
   space and the compositor's; protect against the app freeing in use.
3. Per-process event queue (mouse, keyboard, focus, close, resize), bounded,
   with drop policy documented.
4. Move the kernel-mode apps out: About, Files, Notes, System Monitor become
   ELF programs using a tiny `libarcgui` (window, draw rect/text, event loop).
5. Add `libarcgui` widgets: label, button, text field, list view, scroll view.
6. Terminal app: a window hosting the shell, using pseudo-terminal-like fd pair
   (`SYS_PIPE` and a line discipline).
7. Text editor app: open/save to disk, cursor, selection, basic undo.
8. Process crash handling: if an app faults, close its windows and show a
   small "app quit unexpectedly" notice; the desktop must survive.

Tests:

- Host: event queue (overflow, ordering), surface mapping rules.
- QEMU gate: launch an app from the dock, inject click and typing, assert its
  window content pixels (`tests/boot_test_apps.bat`).
- Stress: open/close 200 windows; no leaks (PMM free count identical after).

Done when: terminal, editor, file manager, and system monitor run as separate
ELF apps and the desktop keeps working when one crashes.

Risks: input latency (measure it), compositor locking (use one compositor
thread and message passing from app syscalls).

## M7: Desktop session, settings, launcher

Goal: it feels like a coherent system, not a demo.

Steps:

1. Settings storage in a config file on disk (`/etc/arc/settings.conf`):
   theme, wallpaper path, resolution, dock contents.
2. Settings app (light/dark, wallpaper picker, display resolution).
3. App registry: `/apps/*.app` manifest (name, icon, executable); the dock and
   a launcher (Spotlight-like search box) read it.
4. Boot sequence: init process starts the compositor session, then restores
   the dock; clean shutdown and reboot paths (`SYS_REBOOT`, `SYS_SHUTDOWN` via
   ACPI where possible).
5. Splash screen, and a graceful failure screen if the session cannot start.

Done when: you can change the theme and wallpaper, reboot, and they persist.

## M8: USB keyboard and mouse (stretch 1)

Goal: input works on laptops and PCs without PS/2.

Steps:

1. xHCI controller bring-up (reset, device context base array, command ring,
   event ring, port status change handling).
2. Device enumeration, control transfers, descriptors.
3. HID class: boot-protocol keyboard and mouse first, then report descriptors
   for the common cases.
4. Hot-plug add and remove. Interrupt-driven (MSI or legacy IRQ).
5. Keep PS/2 as the fallback.

Gate: QEMU `-device qemu-xhci -device usb-kbd -device usb-mouse`
(`tests/boot_test_usb.bat`); then the real PC.

Cut rule: if xHCI is not working after 8 weeks, ship with legacy USB
emulation (BIOS PS/2 emulation) and document the limitation.

## M9: Networking (stretch 2)

Goal: ping, DNS, and a simple HTTP fetch.

Steps:

1. NIC driver: virtio-net (QEMU, easy), then one real NIC (Intel e1000/e1000e
   or Realtek RTL8139/8168, whichever your test PC has).
2. Network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP client, DNS client.
   Port a small stack (lwIP) behind a clean interface, or write a minimal one;
   decide in an ADR before starting.
3. TCP (client side first): connect, send, receive, close, retransmit timers.
4. Socket syscalls: `SYS_SOCKET`, `SYS_CONNECT`, `SYS_SEND`, `SYS_RECV`,
   `SYS_CLOSE` (reuse the fd layer).
5. Userland tools: `ping`, `nslookup`, `fetch` (HTTP GET to a file).
6. Safety: bounds checking on every packet field; fuzz the parsers.

Gate: QEMU user-mode networking; ping the gateway, resolve a name, fetch a
page from a local server (`tests/boot_test_net.bat`).

Cut rule: stop at DHCP + DNS + UDP + ICMP if TCP is not stable after 8 weeks.

## M10: SMP and SIMD (stretch 3)

Goal: use all CPU cores and SIMD for the compositor, safely.

Prerequisites: stable M6. Write a new ADR superseding ADR-004 (FPU state is now
saved) and an ADR for the SMP locking model.

Steps:

1. FPU/SIMD state: enable `CR4.OSFXSR`, `OSXMMEXCPT`, and XSAVE if present;
   save/restore on context switch (lazy or eager, decide in the ADR).
2. Restrict SIMD code to `gfx_simd.c` compiled with explicit target flags;
   scalar fallback for every function. Replace the no-vector gate with a gate
   that fails if vector instructions appear outside `gfx_simd.c`.
3. SIMD kernels: fill, premultiplied-alpha blend, blit, box blur. Test each
   against the scalar version (bit-exact or within 1 LSB).
4. SMP bring-up: parse ACPI MADT, send INIT/SIPI, per-CPU GDT/IDT/stack,
   per-CPU data (`%gs`), TLB shootdown IPI.
5. Locking: introduce spinlocks, then audit every global in the kernel; add a
   lock-order document. Per-CPU run queues keep the MLFQ policy; flag-off for
   old tests.
6. Parallel compositor: split the screen into horizontal bands, one per core,
   join before present.
7. System Monitor shows per-core load.

Gate: `tests/boot_test_smp.bat` with `-smp 4`; stress test with all cores busy
and the GUI active; assert no deadlock or corruption over 10 minutes.

Cut rule: ship SIMD-only (single core) if SMP is not stable after 8 weeks.

## M11: Hardening and performance

Steps:

1. Run the syscall fuzzer and filesystem fuzzer for hours; fix every crash.
2. Add kernel assertions, a panic handler with stack trace, and a watchdog.
3. Memory leak check: PMM free count after open/close and process churn.
4. Performance pass: boot time, frame time, input latency, disk throughput;
   record before/after in `docs/performance.md`.
5. Security pass: validate all user pointers, check privilege boundaries,
   remove debug backdoors.

Done when: a 24-hour soak test (QEMU) completes with no crash and no leak.

## M12: Release, documentation, demo, report

Steps:

1. Build a release image (ISO and USB image) with a version number and
   checksums.
2. Write `docs/user-guide.md` (install, boot, apps) and
   `docs/developer-guide.md` (build, add an app, syscall reference).
3. Record a 3-minute demo video on the real PC.
4. Write the project report: architecture, decisions (ADR summaries), tests,
   benchmarks, limitations, future work.
5. Publish the repository with a clear README, license, and screenshots.

---

## 4. Test and gate inventory (what you will have at the end)

| Area | Host tests | QEMU gate |
|------|------------|-----------|
| Graphics | `test_gfx.c` | `boot_test_gui.py` |
| Window manager | `test_gui.c` | `boot_test_gui.py` |
| Boot (Multiboot2) | parser tests | `boot_test_mb2.bat` |
| Storage | `test_fat32.c`, `test_blockdev.c` | `boot_test_persist.bat` |
| ELF/userspace | `test_elf.c` | `boot_test_elf.bat` |
| Apps | event queue tests | `boot_test_apps.bat` |
| USB | descriptor parser tests | `boot_test_usb.bat` |
| Network | packet parser fuzz tests | `boot_test_net.bat` |
| SMP/SIMD | SIMD-vs-scalar tests | `boot_test_smp.bat` |
| Existing | all current suites | `run_all.bat`, fork, exec gates |

## 5. Pending decision records (write before the work they unlock)

| ADR | Topic | Needed before |
|-----|-------|---------------|
| ADR-007 | Display path: DISPI (QEMU) and Multiboot2/GOP (hardware) | M1/M3 |
| ADR-008 | Block cache and filesystem choice (FAT32 first) | M4 |
| ADR-009 | Syscall ABI versioning and `copy_from_user` rules | M5 |
| ADR-010 | GUI IPC and compositor threading model | M6 |
| ADR-011 | Network stack: port lwIP or write minimal | M9 |
| ADR-012 | Supersede ADR-004: FPU/SIMD state and SIMD scope | M10 |
| ADR-013 | SMP locking model and lock ordering | M10 |

## 6. Cut list (apply in this order when behind schedule)

1. Per-core parallel compositing and GPU backends (drop first).
2. TCP and everything above it in M9 (keep DHCP, DNS, UDP, ICMP).
3. USB beyond boot-protocol keyboard and mouse.
4. SIMD (keep scalar compositor).
5. Number of apps (keep terminal, editor, file manager).
6. Animations and blur (keep the layout and shadows).

Never cut: real-hardware boot, persistent filesystem, ELF userspace, the
release and report milestone.

## 7. Weekly routine

- Monday: pick the next 2 to 3 steps from the current milestone; write them in
  `tasks/todo.md`.
- Mid-week: implement with tests; one commit per step, message format
  `feat(area): ...` or `fix(area): ...` as in the current history.
- Friday: run `tests\run_all.bat` and the milestone gate; update
  `tasks/todo.md`; push.
- End of each month: update the calendar in section 3 with actual hours; if
  more than 20 percent behind, apply the cut list.

## 8. Real-hardware checklist (use at the end of every milestone from M3)

- Firmware mode noted (UEFI or Legacy/CSM) and Secure Boot disabled.
- Boots from USB; time to desktop measured.
- Screen resolution and color correct; no tearing.
- Keyboard and mouse work (PS/2 or USB).
- Disk detected; read and write tested only on a test disk.
- Reboot and shutdown work.
- Result and any fixes recorded in `tasks/hardware-log.md`.

## 9. Per-milestone prompt template (for your coding agent)

```text
You are working on ARC OS (repo: IamWedie/ARC_os). Implement milestone <ID>
from docs/specs/arc-os-24-month-roadmap.md, steps <N..M> only.

Rules:
- Follow section 2 of the roadmap (append-only ABI, ADR before relaxing a
  decision, all gates green, new behavior gets new tests).
- Read the relevant spec and ADRs first, then write a short design note.
- Work step by step; after each step, run host tests, the full build,
  tests\run_all.bat, and this milestone's gate.
- Stop after each step and report: files changed, tests added, commands run,
  results. Do not continue without approval.
- If a step needs a frozen item to change, stop and ask.
```
