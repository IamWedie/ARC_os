# Risk Log

Track risks, mitigations, and status across milestones.

| Date | Milestone | Risk | Impact | Likelihood | Mitigation | Owner | Status | Notes |
|------|-----------|------|--------|------------|------------|-------|--------|-------|
| 2026-11-22 | M1 | `-vga none -device bochs-display,vgamem=67108864` disables legacy DISPI I/O ports 0x1CE/0x1CF on QEMU 11.1.0 (spike verified: mode readback stayed 0; default device works). | M1 step 1-2 blocked if taken literally | High | Use Bochs DISPI MMIO (BAR2) or compatible device flags; verify with screendump before committing. Document in ADR-007. | dev | Open | Spike: 1920x1080 works on default device; bochs-display device needs MMIO path. |
