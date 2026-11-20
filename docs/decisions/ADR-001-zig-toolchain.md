# ADR-001: Use Zig as the freestanding C compiler and linker

## Status
Accepted

## Date
2026-11-18

## Context

ARC OS builds on Windows without an MSYS2 shell. The kernel must be a
freestanding x86_64 ELF image linked at `0x200000`, and the boot stub must be
a 32-bit ELF with a Multiboot1 header.

The installed MinGW GCC (`C:\msys64\ucrt64\bin\gcc.exe`) targets Windows and
emits PE/COFF; its linker cannot produce the required ELF images. The MSYS2
Clang installation is unusable in this environment, and no x86_64-elf
cross-GCC is installed.

## Decision

Use Zig 0.16 (`zig cc`) with `-target x86_64-freestanding` for kernel and user
objects, and `-target x86-freestanding` for the 32-bit boot stub. Zig bundles
Clang and LLD, so it emits ELF directly. GNU objcopy remains the tool for
ELF-to-binary conversion and for embedding raw blobs.

The build script discovers Zig from `ZIG`, `PATH`, a known local install, or
the WinGet package tree, and fails loudly if no working binary is found.

## Alternatives Considered

### MinGW GCC
- Pros: Already installed; no extra dependency.
- Cons: Emits PE/COFF and cannot link the ELF64 kernel.
- Rejected: Cannot produce the required artifact.

### MSYS2 Clang / ld.lld
- Pros: Present under `C:\msys64\ucrt64\bin`.
- Cons: `clang.exe --version` fails silently in this environment.
- Rejected: Not reliably executable.

### x86_64-elf cross-GCC
- Pros: Traditional bare-metal toolchain.
- Cons: Not installed; another large toolchain to provision.
- Rejected for now: Zig already provides Clang/LLD in one binary.

## Consequences

- The build requires Zig 0.16+ and GNU objcopy.
- `-fno-builtin`, `-fno-stack-protector`, `-mno-red-zone`, and
  `-mno-mmx -mno-sse -mno-sse2 -mno-avx` are part of the kernel contract.
- `-mgeneral-regs-only` is not sufficient on x86: Clang still auto-vectorized
  `terminal_clear` to SSE2, which faults with `CR4.OSFXSR` clear. The build
  now scans the linked text for vector/MMX instructions and fails if any exist
  (see ADR-004).
- The userspace shell links at `0x8000000000` and therefore needs
  `-mcmodel=large`.
- Toolchain discovery is explicit and testable, so CI fails with a useful
  message instead of silently building the wrong stub.
