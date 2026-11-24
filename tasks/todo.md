# TODO

## Current: M1 — 1080p framebuffer, font, double buffering

- [x] M1 design note + DISPI/MMIO spike confirmation (ADR-007, risk-log)
- [x] Step 1: gfx_init max-res detect/readback, 1920x1080x32 request, fallbacks
- [x] Step 2: QEMU args `-vga none -device bochs-display,vgamem=67108864` (MMIO BAR2 fallback)
- [x] Step 3: RAM back buffer from PMM
- [x] Step 4: gfx_present dirty-rect copy + merge (host-testable)
- [x] Step 5: OFL Inter pre-rasterized font (12/14/18/24), coverage alpha blend
- [x] Step 6: premultiplied-alpha blend/fill primitives (scalar)
- [x] Step 7: 60 Hz LAPIC frame pacer, skip clean frames
- [x] Host tests: tests/test_gfx.c suite 12
- [x] QEMU gate: tests/boot_test_gui.py screendump pixel assertions
- [x] All gates green, commit, review, merge/tag

## Current: M2 — Mouse, compositor, windows, dock, menu bar (kernel-mode)

- [x] Step 1: PS/2 mouse driver (IRQ12, 3-byte packets, clamp, accel) + host tests
- [x] Step 2: compositor core (16 windows, Z-order, premul blend, damage, hit-test)
- [x] Step 3: basic visual layer (rounded corners, title bar, traffic lights)
- [x] Step 4: menu bar (translucent, 28 px, logo/app name/clock)
- [x] Step 5: dock (centered pill, hover magnification, running indicator)
- [x] Step 6: animations (ease-out/spring open/close/minimize)
- [x] Step 7: built-in apps (About, Notes, Files, System Monitor)
- [x] Step 8: keyboard routing (focused text field gets keys, else shell)
- [x] Host tests: tests/test_gui.c suite 13 extended
- [x] QEMU gate: tests/boot_test_gui.py desktop screendump assertions
- [x] M2 interaction gate: mouse move/click/window appear/drag/close/dock hover
- [x] All M2 gates green, commit, review, merge/tag

## Done: M3 — Real-hardware boot path

- [x] MB2+MB1 headers/parser, normalized bootinfo, boot_fb+bochs_dispi fallback, PMM hardening, on-screen debug/panic, USB image builder, boot_test_mb2, host bootinfo tests. Gates green. Integrated to main, tagged `m3`.

## Current: M4 — Persistent storage

- [x] M4.1 PCI enumeration (`kernel/pci.c`): scan buses, record vendor/device/class/BARs/IRQ
- [x] M4.2 Block layer (`kernel/blockdev.c`): read/write/flush, small write-through block cache
- [x] M4.3 Wrap existing ATA PIO in block interface; keep as QEMU baseline
- [~] M4.4 AHCI driver (`kernel/ahci.c`): controller detection/BAR5/IRQ logging (best-effort); full DMA command engine deferred, ATA PIO baseline remains
- [x] M4.5 Partition tables: MBR/GPT parsing (read-only)
- [x] M4.6 FAT32: read, write, directories, SFN + LFN read/write
- [x] M4.7 VFS: mount table, path resolution, vfs ops dispatch; ARCFS and FAT32 plug in; fd layer unchanged
- [x] M4.8 Syscalls 17+: SYS_STAT, SYS_READDIR, SYS_MKDIR, SYS_UNLINK, SYS_MOUNT; documented in `docs/specs/syscall-abi.md`
- [x] M4.9 Boot/load first user app from disk feasible: FAT32-backed FS dispatch + embedded hello.bin installed to mounted FS; ARCFS RAM recovery path retained when no disk
- [~] M4.10 Crash-safety basics: write-through blockdev, basic metadata ordering in FAT32 paths, fsck-like tool `tools/fsck_fat32.py`; full journaling/ordered-write guarantees deferred
- [x] Host tests: FAT32 parser/ops/LFN/corrupt/fuzz on in-memory image (65 checks pass)
- [x] QEMU gate: `tests/boot_test_persist.bat` - create file, reboot, read back
- [x] Real PC: documented in hardware-log (deferred if no hardware)
- [x] Required gates green: `build_kernel.py --quiet`, `run_tests.bat` (15 suites), `run_all.bat`, `boot_test_gui/fork/exec/mb2/persist`
- [ ] M4 commit/integration (requires explicit user choice)
