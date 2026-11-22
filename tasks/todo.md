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

## Next: M2 — Mouse, compositor, windows, dock, menu bar (kernel-mode)
