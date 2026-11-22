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
- [ ] Step 6: animations (ease-out/spring open/close/minimize)
- [x] Step 7: built-in apps (About, Notes, Files, System Monitor)
- [ ] Step 8: keyboard routing (focused text field gets keys, else shell)
- [x] Host tests: tests/test_gui.c suite 13 extended
- [x] QEMU gate: tests/boot_test_gui.py desktop screendump assertions
- [ ] M2 interaction gate: mouse move/click/window appear/drag/close/dock hover
- [ ] All M2 gates green, commit, review, merge/tag
