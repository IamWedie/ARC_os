# ADR-003: Two-stage boot image with an embedded ELF64 kernel

## Status
Accepted

## Date
2026-11-18

## Context

QEMU/GRUB Multiboot1 expects a 32-bit ELF image with a header in the first
8 KiB. The kernel is a 64-bit ELF linked at `0x200000`. The previous build
only compiled a boot stub, and LLD later dropped the unreferenced kernel blob
from the boot image entirely.

## Decision

Build two images:

1. `os_kernel.elf` — ELF64 kernel at `0x200000`, including the raw ring-3
   shell blob in a `.userblob` section.
2. `os.elf` — ELF32 Multiboot1 boot image at `0x100000` with the raw kernel
   blob in a `.kernelblob` section placed at `0x200000`.

`linker/boot32.ld` uses `KEEP(*(.kernelblob))`, and the boot link passes
`--no-gc-sections`, so the blob cannot be garbage-collected. `boot32.S` sets
`%esp` to its own `.bss` stack before calling any code. `entry64.S` reloads
the data segments, zeroes the kernel BSS, and calls `kernel_main`.

The build script validates:
- `os.elf` is ELF32/x86 with a checksum-correct Multiboot header in the first
  8 KiB.
- A `PT_LOAD` segment covers `0x200000` and contains byte-identical kernel
  bytes.
- `os_kernel.elf` is ELF64 with a non-zero entry point.

## Alternatives Considered

### One ELF64 Multiboot image
- Pros: Single link step.
- Cons: Multiboot1 loaders expect 32-bit ELF; a 64-bit entry would still need
  a mode transition.
- Rejected: The two-stage split keeps the loader format conventional.

### GRUB modules
- Pros: No blob embedding; GRUB loads the kernel as a module.
- Cons: Adds a bootloader dependency and module layout work.
- Rejected for now: Direct `-kernel os.elf` boot is simpler.

### Flat binary + custom disk bootloader
- Pros: No ELF loader requirements.
- Cons: Loses ELF segments/symbols; requires BIOS disk code and a boot sector.
- Deferred: The standalone boot experiments are not part of the current build.

## Consequences

- The build needs objcopy to convert and embed blobs.
- Stale-blob bugs are caught by the byte-for-byte validation step.
- `os.elf` is about 62 KB; the raw kernel blob is about 39 KB.
- The kernel BSS is not loaded from the image; `entry64.S` must zero it before
  C code runs.
