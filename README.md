# ARC_OS - Operating System Built from Scratch

<p align="center">
  <img src="https://img.shields.io/badge/OS-ARC OS%20v1.0-blue" alt="ARC OS v1.0">
  <img src="https://img.shields.io/badge/Arch-x86_64-blue" alt="x86_64">
  <img src="https://img.shields.io/badge/Boot-Multiboot%20%26%20BIOS-green" alt="Multiboot + BIOS">
</p>

A work-in-progress x86_64 operating system built entirely from scratch — no Linux, no Windows, no Unix. The verified foundation now includes a size-classed slab heap, a preemptive four-level MLFQ scheduler, and an interactive ring-3 shell; the filesystem/POSIX layer is next.

---

## Table of Contents

- [About](#about)
- [Architecture](#architecture)
- [Features](#features)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Running](#running)
- [How It Works](#how-it-works)
- [VGA Text Mode](#vga-text-mode)
- [Shell Commands](#shell-commands)
- [Extending the OS](#extending-the-os)
- [FAQ](#faq)
- [Contributing](#contributing)
- [License](#license)

---

## About

ARC OS (ARC_OS) is a hobby operating system written entirely in **x86_64 Assembly** and **C** from scratch. It demonstrates the fundamental concepts of operating system development including:

- **Bootloader development** — Multiboot1 compliant, switches from 16-bit real mode to 32-bit protected mode to 64-bit long mode
- **Memory management** — physical frame allocator, identity-mapped page tables, size-classed slab heap
- **Interrupt handling** — PIC/IOAPIC routing, LAPIC timer, keyboard IRQs, and COM1 receive interrupts
- **Process scheduling** — preemptive four-level MLFQ scheduler with explicit process states and an idle fallback
- **Device drivers** — VGA text console, PS/2 keyboard, interrupt-driven COM1 serial
- **Filesystem** — In-memory file system with open/read/close/list operations
- **System calls** — write, read, exit, open, close, ls, delete, seek, size, truncate, rename, clear
- **Userspace** — Interactive ring-3 shell with command parsing, usable over VGA or serial

This project is an educational exploration of OS internals. No existing operating system code was used.

---

## Architecture

```
┌─────────────────────────────────────────────┐
│              BOOT PROCESS                     │
├─────────────────────────────────────────────┤
│  BIOS → GRUB (Multiboot1)                   │
│  → Load kernel at 0x100000                  │
│  → 16-bit Real Mode                         │
│  → Load GDT                                 │
│  → Enable PAE                               │
│  → Set up Page Directory                    │
│  → Enable EFER.LME (Long Mode)              │
│  → Enable Paging                            │
│  → Far Jump to 64-bit Code                  │
│  → 64-bit Long Mode                         │
│  → Set Data Segments                        │
│  → Load IDT                                 │
│  → Call kernel_main()                       │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│              KERNEL STATE                     │
├─────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │  VGA     │  │  GDT/IDT │  │ Scheduler │ │
│  │  Console │  │          │  │           │ │
│  │  80x25   │  │  256     │  │ 4-level   │ │
│  │  Text    │  │  Entries │  │ MLFQ, 16  │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │ Keyboard │  │  Memory  │  │ Filesystem│ │
│  │ + COM1   │  │  Slab+PMM│  │ In-Memory │ │
│  │ IRQ1/4   │  │          │ │           │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐                 │
│  │ PIC/     │  │  Syscalls│                 │
│  │ IOAPIC   │  │  write   │                 │
│  │ Timer    │  │  read    │                 │
│  │ IRQ0/40  │  │  exit    │                 │
│  └──────────┘  └──────────┘                 │
└─────────────────────────────────────────────┘
```

---

## Features

- Multiboot1-compliant boot image — `os.elf` is an ELF32 loader with an embedded ELF64 kernel
- 64-bit long mode — 32-bit boot stub builds PAE page tables and enters long mode
- VGA text console — 80x25 characters, 16 colors, mirrored to COM1 serial
- Keyboard driver — scancode-to-ASCII mapping and a ring buffer
- Serial input — IRQ-driven COM1 receive ring, so the shell works headlessly
- Preemptive timer ticks — PIT calibration plus LAPIC timer (vector 0x40)
- Four-level MLFQ scheduler — O(1) FIFO run queues, 10-tick quantum, 1-second priority boost
- Explicit process lifecycle — `READY`, `RUNNING`, `BLOCKED`, `ZOMBIE`, plus a non-queued idle process
- Physical + virtual memory — multiboot memory map, 2 MiB identity map, 4 KiB page mapping
- Slab heap — 12 size classes (16 B–32 KiB), PMM fallback for large allocations
- Ring-3 user shell — loaded from an embedded blob and entered via `iretq`
- Owned process frames — user code/stack runs are tracked per process and freed automatically by the idle reaper
- System call gate — `int 0x80` for write, read, exit, open, close, ls, delete, seek, size, truncate, rename, clear
- RAM-backed filesystem — file metadata plus a block arena
- Build pipeline — Zig/lld + GNU objcopy, no MSYS2 shell required
- Validated images — build fails unless the multiboot header and embedded kernel bytes are correct

---

## Project Structure

```
ARC_os/
├── boot/
│   ├── boot32.S              # Multiboot1 entry, PAE tables, long-mode jump
│   ├── entry64.S             # 64-bit entry, BSS zeroing, kernel_main call
│   ├── boot.S / os.S         # Earlier standalone-boot experiments (not built)
├── kernel/
│   ├── kernel.c              # Main kernel entry point
│   ├── kernel.h              # Kernel-wide declarations
│   ├── console.c             # VGA text mode console driver
│   ├── serial.c              # COM1 output + IRQ-driven receive ring
│   ├── gdt.c                 # GDT + TSS
│   ├── idt.c                 # Interrupt Descriptor Table
│   ├── interrupts.c          # PIC, keyboard, IRQ wiring
│   ├── isr.S                 # Interrupt/exception/syscall stubs
│   ├── exceptions.c          # CPU exception dump + halt
│   ├── pmm.c                 # Physical frame allocator
│   ├── vmm.c                 # Page tables and mappings
│   ├── slab.c / slab.h       # Size-classed slab allocator
│   ├── memory.c              # kmalloc/kfree + heap self-test
│   ├── scheduler.c           # MLFQ scheduler, process states, context switching
│   ├── syscall.c             # int 0x80 dispatcher
│   ├── user.c                # Ring-3 shell loader
│   ├── fs.c                  # RAM-backed filesystem
│   ├── timer.c               # PIT driver
│   ├── lapic.c               # Local APIC + LAPIC timer
│   ├── ioapic.c              # IOAPIC routing
│   ├── acpi.c                # ACPI/MADT discovery
│   └── ata.c                 # ATA PIO driver
├── lib/
│   ├── string.c              # memset/memcpy/memmove/strlen/strcmp
│   └── stdlib.c              # malloc/free over kmalloc/kfree
├── userspace/
│   ├── shell.c               # Ring-3 shell
│   └── crt0.S                # User entry stub
├── linker/
│   ├── kernel64.ld           # ELF64 kernel at 0x200000
│   └── boot32.ld             # ELF32 boot image; embeds kernel at 0x200000
├── tools/
│   ├── build_kernel.py       # Full build + image validation
│   └── boot_test.py          # QEMU serial test (TCP transport, --input/--expect)
├── tests/
│   ├── run_tests.bat         # Host slab/memory + scheduler suites
│   ├── run_all.bat           # Full gate: host tests + build + interactive boot
│   ├── test_slab_stress.c    # Links the real slab.c + memory.c
│   └── test_scheduler.c      # Links the real scheduler.c
├── docs/decisions/           # Architecture decision records
├── docs/specs/               # Implementation specs (process resource ownership)
├── tasks/                    # Plan and task tracker
├── os.elf                    # Output: bootable multiboot image
├── build.bat                 # Full build
├── build.py                  # Python entry point
├── rebuild.bat               # Clean rebuild
├── run.bat                   # Boot os.elf in QEMU
└── README.md
```

---

## Decision Records

Significant technical choices and their trade-offs are recorded in
[`docs/decisions/`](docs/decisions/):

- [ADR-001: Zig toolchain](docs/decisions/ADR-001-zig-toolchain.md)
- [ADR-002: Slab allocator](docs/decisions/ADR-002-slab-allocator.md)
- [ADR-003: Two-stage boot image](docs/decisions/ADR-003-boot-image.md)
- [ADR-004: No SSE/MMX until FPU context switching](docs/decisions/ADR-004-no-sse.md)
- [ADR-005: Four-level MLFQ scheduler](docs/decisions/ADR-005-mlfq-scheduler.md)
- [ADR-006: Interrupt-driven COM1 receive and TCP test transport](docs/decisions/ADR-006-serial-receive.md)

---

## Prerequisites

### Required Tools

1. **Zig 0.16** — Used as the C compiler and linker. Zig ships a complete
   Clang/LLD toolchain and can emit freestanding x86_64 ELF objects, which the
   MinGW GCC cannot do.

   - Download: https://ziglang.org/download/
   - Confirmed working: `C:\Users\wadia\zig\ziglang\zig.exe` (0.16.0)
   - `build_kernel.py` also searches `PATH`, `ZIG`, and the WinGet package tree.

2. **GNU objcopy** — Converts ELF images to raw binaries and embeds blobs.
   The native Windows binary from MSYS2 is fine; no MSYS2 shell is needed.

   - Confirmed working: `C:\msys64\ucrt64\bin\objcopy.exe` (binutils 2.45)

3. **Python 3** — Runs `tools/build_kernel.py` and the test harnesses.

4. **QEMU** (optional, for runtime verification) — Boots `os.elf`.

   ```cmd
   winget install SoftwareFreedomConservancy.QEMU
   ```

### Quick Setup

```cmd
git clone https://github.com/IamWedie/ARC_os.git
cd ARC_os
build.bat
```

---

## Building

| Command | Description |
|---------|-------------|
| `build.bat` | Compile the kernel, ring-3 shell, and boot image |
| `build.bat --quiet` | Same build with command echoes suppressed |
| `python build.py` | Python entry point for the same build |
| `rebuild.bat` | Delete cached objects and rebuild from scratch |
| `tests\run_tests.bat` | Host tests for the real slab/memory and scheduler code |
| `tests\run_all.bat` | Full gate: host tests + build + interactive QEMU shell test |
| `python tools\boot_test.py` | Boot in QEMU and validate serial markers; `--input` drives shell commands |

The build writes `os.elf` in the repository root. It fails (non-zero exit)
if any object fails to compile, the kernel does not link, the multiboot
header is missing/malformed, the embedded kernel bytes do not match, or the
kernel contains vector/MMX instructions (SSE is not enabled at boot).

Intermediate objects live in `%TEMP%\opencode\arc_build`.

### Output Files

| File | Description | Approx. size |
|------|-------------|--------------|
| `os.elf` | ELF32 multiboot boot image with embedded ELF64 kernel | ~68 KB |
| `<build>\kernel.bin` | Raw kernel blob embedded at physical `0x200000` | ~46 KB |
| `<build>\user_shell.bin` | Raw ring-3 shell blob | ~7 KB |

---

## Running

### Run Script

```cmd
run.bat
```

### Direct QEMU Boot

```cmd
qemu-system-x86_64 -kernel os.elf -nographic -m 512M
```

`-nographic` maps COM1 to the terminal. The shell reads typed input through the
COM1 receive interrupt; commands such as `help`, `ls`, and `exit` work without
a graphical console.

### Automated Boot Check

```cmd
python tools\boot_test.py --timeout 30 --expect "arc> " ^
    --expect "runtime.txt" --input "help" --input "touch runtime.txt" ^
    --input "ls" --input "exit"
```

The test boots `os.elf`, waits for the banner and prompt, fails on panic
markers, feeds each `--input` line after a prompt appears, and prints the
captured serial console.

It uses QEMU's **TCP serial backend**, not `-serial stdio`. QEMU's Windows stdio
backend discards input bytes whenever the emulated 16550 advertises no receive
capacity, which truncates piped commands. TCP applies backpressure instead, so
the test exercises the real driver without losing bytes. Manual interactive use
can still use `-nographic`; human typing never fills the FIFO fast enough to hit
that path. See [ADR-006](docs/decisions/ADR-006-serial-receive.md) for details.

### QEMU Tips

- `-nographic` — Serial console in the terminal (Ctrl+A X to quit)
- `-m 512M` — Allocate 512 MB RAM
- `-display gtk` — Use the graphical VGA window instead
- `-snapshot` — Don't persist disk changes

---

## How It Works

### Boot Sequence

1. QEMU/GRUB reads the Multiboot header from the ELF32 `os.elf`
2. The boot stub is loaded at `0x100000` along with the embedded kernel blob
   at `0x200000`
3. `boot32.S` (`_start`) runs in 32-bit protected mode:
   - Sets `%esp` to its own `.bss` stack (Multiboot does not guarantee one)
   - Loads the boot GDT
   - Enables PAE and points `CR3` at a 2 MiB identity map
   - Sets `EFER.LME`, enables paging, and far-jumps into 64-bit code
4. `entry64.S` (`_entry64`) runs in long mode:
   - Reloads all data segments from the boot GDT
   - Zeroes the kernel `.bss` and sets the kernel stack
   - Calls `kernel_main(multiboot_info)`
5. `kernel_main()` initializes:
   - VGA console and COM1 serial
   - PMM, VMM, and the early slab heap
   - ACPI, GDT/TSS, IDT, PIC, LAPIC, IOAPIC, and PIT
   - Scheduler, ATA, the RAM-backed filesystem, and COM1 receive routing
   - Runs the heap self-test
   - Loads the ring-3 shell as a user process; PID 0 becomes the idle fallback
6. The LAPIC timer preempts the idle stack into the user shell; PID 0 is never
   queued, so it cannot compete with runnable work
7. The shell performs I/O through `int 0x80`; a blocking read halts until a
   keyboard or COM1 receive interrupt wakes the scheduler

### Memory Map

| Address | Purpose |
|---------|---------|
| `0x0000000000000000` – `0x00000000000FFFFF` | Low memory, BIOS data, VGA, boot stub |
| `0x0000000000100000` | Boot stub load address |
| `0x0000000000200000` | 64-bit kernel load address |
| `0x00000000000B8000` | VGA text buffer |
| `0x0000008000000000` | Ring-3 shell code mapping |
| `0x0000009000000000` | Ring-3 shell stack mapping |

The VMM identity-maps available RAM with 2 MiB pages and allocates 4 KiB page
tables for the user mappings above.

---

## VGA Text Mode

### Memory Layout

- **Base Address**: `0xB8000`
- **Format**: Each character = 2 bytes
  - Byte 0: ASCII character
  - Byte 1: Color attribute (foreground | background << 4)
- **Resolution**: 80 columns × 25 rows
- **Total Size**: 80 × 25 × 2 = 4000 bytes

### Color Codes

| Code | Color | Code | Color |
|------|-------|------|-------|
| 0 | Black | 8 | Dark Grey |
| 1 | Blue | 9 | Light Blue |
| 2 | Green | 10 | Light Green |
| 3 | Cyan | 11 | Light Cyan |
| 4 | Red | 12 | Light Red |
| 5 | Magenta | 13 | Light Magenta |
| 6 | Brown | 14 | Light Brown |
| 7 | Light Grey | 15 | White |

---

## Shell Commands

| Command | Description | Example |
|---------|-------------|---------|
| `help` | Show available commands | `help` |
| `clear` | Clear the screen | `clear` |
| `ls` | List files in the filesystem | `ls` |
| `echo <text>` | Print text to screen | `echo Hello World` |
| `cat <file>` | Print a file | `cat notes.txt` |
| `touch <file>` | Create an empty file | `touch notes.txt` |
| `rm <file>` | Delete a file | `rm notes.txt` |
| `cp <src> <dst>` | Copy a file | `cp a.txt b.txt` |
| `mv <src> <dst>` | Rename/move a file | `mv a.txt b.txt` |
| `truncate <file>` | Truncate a file to zero length | `truncate notes.txt` |
| `exit` | Exit the shell | `exit` |

---

## Extending the OS

### Short Term
- Graphical mode — VBE/VESA framebuffer driver
- Font rendering — Bitmap font for graphics mode
- Mouse driver — PS/2 mouse with cursor sprite
- GUI framework — Windows, buttons, dialogs
- Installer — Windows-style multi-step setup

### Medium Term
- USB support — USB host controller driver
- Networking — NIC driver + TCP/IP stack
- FAT32 driver — Read/write external storage
- Dynamic linking — ELF loader for shared libraries
- Process management — fork/exec/wait and IPC (preemption and MLFQ already exist)

### Long Term
- Windowing system — Multiple overlapping windows
- File manager — Graphical file browser
- Text editor — Built-in editor
- Package manager — Install additional software
- Display driver — Multiple resolution support
- Memory management — Demand paging, copy-on-write

---

## FAQ

**Q: What architecture is this for?**
A: x86_64 (64-bit Intel/AMD). The code uses 64-bit registers and long mode instructions.

**Q: Can this boot on real hardware?**
A: The image produces a valid Multiboot1 boot image, but only QEMU is an
intended target today. Real-hardware boot has not been verified.

**Q: Is this a Linux fork?**
A: No. Everything is written from scratch. No Linux, Windows, or Unix code is used.

**Q: What language is the kernel in?**
A: Primarily C with x86_64 assembly for the boot transition, interrupt stubs,
syscall gate, and context-switch contract.

**Q: How does the bootloader work?**
A: `boot32.S` is a 32-bit Multiboot1 stub that creates PAE page tables and
enters long mode, then jumps to the ELF64 kernel embedded at `0x200000`.
`entry64.S` finishes the transition and calls `kernel_main`.

**Q: How does the scheduler work?**
A: Four MLFQ levels, each an O(1) FIFO ring. A running process is demoted one
level after a 10-tick (100 ms) quantum; every 100 ticks (1 s) all ready work
returns to level 0 to bound starvation. PID 0 is a special idle process that is
never queued and runs only when no real process is ready.

**Q: What is the file size?**
A: The kernel ELF is about 204 KB including debug sections; the raw kernel
blob embedded in the boot image is about 46 KB. `os.elf` is about 68 KB.

**Q: Why does the boot test use TCP instead of stdio?**
A: QEMU's Windows stdio backend drops input bytes when the emulated UART
advertises no receive capacity, which truncates piped commands. TCP applies
backpressure, so no bytes are lost. Interactive use still works with
`-nographic`; see [ADR-006](docs/decisions/ADR-006-serial-receive.md).

**Q: Why does the build require Zig?**
A: The kernel must be a freestanding ELF. The native Windows MinGW GCC emits
PE/COFF, and its linker cannot produce the ELF64 image we need. Zig bundles
Clang/LLD and can target `x86_64-freestanding` directly. GNU objcopy is still
used for binary conversion and blob embedding.

**Q: Can I use a different compiler/linker?**
A: Any toolchain that can emit freestanding x86_64 ELF, link with
`linker/kernel64.ld`, and produce an ELF32 boot image with `linker/boot32.ld`
will work. The build script currently locates Zig and GNU objcopy.

---

## Contributing

Contributions are welcome! Here's how to get started:

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/amazing-feature`
3. Make your changes
4. Commit: `git commit -m 'Add amazing feature'`
5. Push: `git push origin feature/amazing-feature`
6. Open a Pull Request

---

## License

This project is licensed under the MIT License. See LICENSE file for details.

---

<p align="center"><b>Built from scratch. No shortcuts.</b></p>
