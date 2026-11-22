# ADR-007: Display path — Bochs DISPI for QEMU dev, firmware framebuffer for real hardware

## Status
Proposed

## Date
2026-11-22

## Context

ARC OS currently renders only 80x25 VGA text. Milestone M1 needs a 1920x1080x32
graphical framebuffer with double buffering. Milestone M3 needs the same desktop
to boot on real hardware.

A 2026-11-22 spike on QEMU 11.1.0 (TCG, default display device) verified:
- Multiboot1 VBE fields are unusable in this boot path (video flag set, all
  framebuffer fields zero).
- Bochs VBE DISPI is programmable via I/O ports `0x1CE` (index) / `0x1CF`
  (data): device ID `0xB0C5`, 1920x1080x32 accepted, readback `0x780x0x438`
  bpp 32, LFB enabled with `0x41`.
- The framebuffer is PCI BAR0 of vendor `0x1234` (Bochs/QEMU display),
  physical base `0xFD000000` in the default configuration.
- Writing pixels to the mapped LFB produced an exact 1920x1080 QEMU
  `screendump` (PPM sampled pixels matched).

The same spike also found an incompatibility with the roadmap's suggested QEMU
args: `-vga none -device bochs-display,vgamem=67108864` moves the LFB to
`0xF8000000` but the legacy DISPI I/O ports `0x1CE/0x1CF` do not respond
(mode readback stayed 0). The `bochs-display` device exposes DISPI registers
through MMIO BAR2; the legacy VGA-ISA I/O port path is not equivalent.

Real hardware has no Bochs DISPI device; it provides a firmware framebuffer via
Multiboot2 framebuffer tag (type 8) or UEFI GOP.

## Decision

- **QEMU dev path (M1/M2):** Program Bochs VBE DISPI to 1920x1080x32. Prefer a
  display-device/QEMU-arg combination that keeps DISPI programmable; if
  `-device bochs-display` is used for larger VRAM, program DISPI through the
  MMIO BAR2 register bank rather than legacy ports `0x1CE/0x1CF`. The chosen
  path must be verified by readback and screendump before commit.
- **Hardware path (M3):** Add a `display_backend` interface. `boot_fb` consumes
  the Multiboot2 framebuffer tag (type 8) or GOP handoff; the compositor sees
  only the interface. Multiboot1 remains until the M3 path is green.
- **VRAM:** 1080p x 4 bytes ≈ 8.3 MiB per buffer. Front buffer lives in VRAM;
  back buffer lives in system RAM unless VRAM is explicitly provisioned and
  verified sufficient for both.

## Alternatives Considered

### Multiboot1 VBE request
- Pros: Simple header flag.
- Cons: Verified unusable in this QEMU boot path (all framebuffer fields zero).
- Rejected for dev path; Multiboot2 replaces it in M3.

### `-vga none -device bochs-display` with legacy I/O ports only
- Pros: 64 MiB VRAM.
- Cons: Legacy DISPI I/O ports did not respond in spike; mode set silently
  ineffective.
- Rejected as-is; acceptable only with MMIO BAR2 programming verified.

### UEFI GOP only
- Pros: Clean modern firmware path.
- Cons: Larger M3 scope; not needed for QEMU M1/M2 bring-up.
- Deferred to M3 as an alternative behind the `display_backend` interface.

## Consequences

- M1 must include a small spike confirming the exact QEMU device/args and DISPI
  programming mode (I/O vs MMIO) before building on it.
- The compositor must not depend on Bochs-specific registers after init; it
  draws to a `gfx_fb`-style back/front buffer abstraction.
- ADR-004 (no SSE) remains in force through M1/M2; M10 requires a new ADR
  before SIMD lands.
