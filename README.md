# ARC_OS - Operating System Built from Scratch

<p align="center">
  <img src="https://img.shields.io/badge/OS-TinyOS%20v1.0-blue" alt="TinyOS v1.0">
  <img src="https://img.shields.io/badge/Arch-x86_64-blue" alt="x86_64">
  <img src="https://img.shields.io/badge/Boot-Multiboot%20%26%20BIOS-green" alt="Multiboot + BIOS">
</p>

A complete x86_64 operating system built entirely from scratch — no Linux, no Windows, no Unix. Every line of code is written by hand.

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

ARC_OS (also known as TinyOS) is a hobby operating system written entirely in **x86_64 Assembly** and **C** from scratch. It demonstrates the fundamental concepts of operating system development including:

- **Bootloader development** — Multiboot1 compliant, switches from 16-bit real mode to 32-bit protected mode to 64-bit long mode
- **Memory management** — Physical page tables with identity mapping
- **Interrupt handling** — Programmable Interrupt Controller (PIC) remapping with timer and keyboard IRQs
- **Process scheduling** — Round-robin scheduler supporting up to 16 concurrent processes
- **Device drivers** — VGA text console, keyboard scancode-to-ASCII mapping
- **Filesystem** — In-memory file system with open/read/close/list operations
- **System calls** — write, read, open, close, ls, help, echo, exit, clear
- **Userspace** — Interactive shell with command parsing

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
│  │  80x25   │  │  256     │  │ Round-Robin│ │
│  │  Text    │  │  Entries │  │ 16 procs  │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │ Keyboard │  │  Memory  │  │ Filesystem│ │
│  │ IRQ1     │  │  Allocator│ │ In-Memory │ │
│  │ Scancode │  │          │ │           │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐                 │
│  │  PIC     │  │  Syscalls│                 │
│  │  Remap   │  │  write   │                 │
│  │  Timer   │  │  read    │                 │
│  │  IRQ0    │  │  exit    │                 │
│  └──────────┘  └──────────┘                 │
└─────────────────────────────────────────────┘
```

---

## Features

- Multiboot1 compliant bootloader — loads via GRUB
- Standalone BIOS bootloader — loads from disk directly
- 64-bit long mode — full x86_64 architecture support
- VGA text console — 80x25 characters, 16 colors
- Keyboard driver — real-time scancode-to-ASCII mapping
- Timer interrupts — periodic scheduler ticks
- Round-robin scheduler — up to 16 concurrent processes
- Physical memory management — page tables + allocator
- In-memory filesystem — open/read/close/list operations
- System call interface — write, read, open, close, ls, help, echo, exit, clear
- Interactive shell — command-line interface with prompt
- PIC remapping — IRQ0 (timer) and IRQ1 (keyboard) remapped
- Multi-file C kernel — modular architecture with headers
- Build system — automated batch build script
- ISO creation — GRUB-based bootable ISO
- Multi-image support — floppy (1.44MB) and hard disk (10MB) images

---

## Project Structure

```
os_project/
├── boot/
│   ├── os.S                  # Multiboot1 entry point (32→64-bit transition)
│   ├── boot.S                # Standalone BIOS bootloader (512 bytes)
│   └── multiboot.h           # Multiboot1 header definition
├── kernel/
│   ├── kernel.c              # Main kernel entry point
│   ├── kernel.h              # All kernel headers and declarations
│   ├── console.c             # VGA text mode console driver
│   ├── gdt.c                 # Global Descriptor Table
│   ├── idt.c                 # Interrupt Descriptor Table
│   ├── interrupts.c          # PIC remapping, timer + keyboard IRQs
│   ├── scheduler.c           # Round-robin process scheduler
│   ├── memory.c              # Physical memory + memset/memcpy/strlen/strcmp
│   ├── syscall.c             # System call implementations
│   └── fs.c                  # In-memory filesystem
├── userspace/
│   └── shell.c               # Interactive command shell
├── lib/
│   ├── string.c              # String utility functions
│   └── stdlib.c              # Standard library functions
├── linker/
│   └── linker.ld             # Linker script (loads at 0x100000)
├── iso/                      # ISO build directory (bootable ISO)
├── os.bin                    # Bootable flat binary (4KB)
├── kernel.bin                # Standalone binary
├── os_floppy.img             # Bootable floppy image (1.44MB)
├── os_harddisk.img           # Bootable hard disk image (10MB)
├── build.bat                 # Build script (compiles everything)
├── make_iso.bat              # Create bootable GRUB ISO
├── make_image.py             # Create disk images
├── run.bat                   # Run with QEMU
├── build.py                  # Python build script
├── README.md                 # This file
└── .gitignore                # Git ignore rules
```

---

## Prerequisites

### Required Tools

1. **MSYS2 UCRT64** — Provides GCC cross-compiler for bare-metal x86_64

   **Installation:**
   - Download MSYS2: https://www.msys2.org/
   - Run MSYS2 UCRT64 terminal
   - Install GCC: `pacman -S mingw-w64-ucrt-x86_64-gcc`
   - GCC should be at `C:\msys64\ucrt64\bin\gcc.exe`

2. **QEMU** — For testing the OS in a virtual machine

   **Installation:**
   - Download QEMU: https://www.qemu.org/
   - Or use winget: `winget install SoftwareFreedomConservancy.QEMU`
   - Add to PATH

### Quick Setup

```cmd
REM Install MSYS2 (https://www.msys2.org/)
REM Open MSYS2 UCRT64 terminal
pacman -S mingw-w64-ucrt-x86_64-gcc

REM Verify GCC
C:\msys64\ucrt64\bin\gcc.exe --version

REM Clone this repo
git clone https://github.com/IamWedie/ARC_os.git
cd ARC_os

REM Build
build.bat
```

---

## Building

### Option 1: Using build.bat (Recommended)

```cmd
C:\Users\wadia\os_project\build.bat
```

This compiles all source files, links them, and creates bootable binaries.

### Option 2: Using build.py

```cmd
C:\msys64\ucrt64\bin\python3.exe build.py
```

### Output Files

| File | Description | Size |
|------|-------------|------|
| `os.bin` | Bootable flat binary (Multiboot1) | ~4KB |
| `kernel.bin` | Standalone binary | ~4KB |
| `boot/os.o` | Compiled object file | ~9KB |

---

## Running

### With QEMU (Direct Kernel Boot)

```cmd
qemu-system-x86_64 -kernel os.bin -nographic -m 512M
```

### With QEMU (Disk Image)

```cmd
qemu-system-x86_64 -drive format=raw,file=kernel.bin -nographic -m 512M
```

### With QEMU (Floppy Image)

```cmd
qemu-system-x86_64 -drive format=raw,file=os_floppy.img -nographic -m 512M
```

### With GRUB ISO

```cmd
make_iso.bat
qemu-system-x86_64 -cdrom os.iso -nographic -m 512M
```

### Run Script

```cmd
run.bat
```

### QEMU Tips

- `-nographic` — No graphical output (uses serial console)
- `-m 512M` — Allocate 512MB RAM
- `-display gtk` — Use GTK display (for graphical output when added)
- `-snapshot` — Don't save changes to disk
- `-boot a` — Boot from floppy
- `-boot c` — Boot from hard disk

---

## How It Works

### Boot Sequence

1. BIOS initializes hardware
2. BIOS loads GRUB bootloader
3. GRUB loads kernel at 0x100000 (1MB) in 32-bit protected mode
4. GRUB passes Multiboot info in EAX
5. _start (entry point) executes:
   - Set up stack in 32-bit mode
   - Load GDT (Global Descriptor Table)
   - Enable PAE (Page Address Extension)
   - Set up identity-mapped page directory
   - Enable EFER.LME (Long Mode Enable)
   - Enable paging (CR0.PG)
   - Far jump to flush prefetch → 64-bit mode
6. In 64-bit mode:
   - Set data segment registers (DS, ES, FS, GS, SS)
   - Set up stack pointer
   - Load IDT (Interrupt Descriptor Table)
   - Call kernel_main(multiboot_info)
7. kernel_main() initializes:
   - VGA console
   - GDT
   - IDT
   - PIC (timer + keyboard interrupts)
   - Scheduler
   - Filesystem
   - Launch shell
8. Shell runs interactive command loop
9. When idle, CPU executes hlt instruction

### Memory Map

| Address | Purpose |
|---------|---------|
| 0x000000 - 0x0009FFFF | Low memory (BIOS data) |
| 0x00100000 | Kernel load address |
| 0x00100000 - 0x001FFFFF | Kernel code/data |
| 0x90000000 - 0x900FFFFF | User process stacks |
| 0xB8000 | VGA text buffer |
| 0x100000000+ | Higher half (kernel space) |

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
| `ls` | List files in filesystem | `ls` |
| `echo <text>` | Print text to screen | `echo Hello World` |
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
- Multitasking — Preemptive scheduling with IPC

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
A: Yes, with proper bootloader (GRUB or a custom BIOS bootloader). Currently tested in QEMU.

**Q: Is this a Linux fork?**
A: No. Everything is written from scratch. No Linux, Windows, or Unix code is used.

**Q: What language is the kernel in?**
A: Primarily C with x86_64 Assembly for low-level operations (bootloader, context switching, interrupt handling).

**Q: How does the bootloader work?**
A: Two bootloaders are included: Multiboot1 (loaded by GRUB) and Standalone BIOS (loads from disk sectors).

**Q: What is the file size?**
A: The compiled kernel is approximately 4KB (os.bin). The total source code is around 2000+ lines across all files.

**Q: Why does the build fail on some systems?**
A: The build requires MSYS2 GCC configured for bare-metal x86_64 compilation. Standard Windows GCC produces PE executables, not ELF. Use the MSYS2 toolchain.

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
