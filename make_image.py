#!/usr/bin/env python3
"""Stage os.elf into an ISO tree and create disk images."""
import os
import shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_ELF = os.path.join(ROOT, "os.elf")
ISO = os.path.join(ROOT, "iso")

if not os.path.isfile(OS_ELF):
    raise SystemExit("os.elf not found; run build.bat first")

os.makedirs(os.path.join(ISO, "boot", "grub"), exist_ok=True)
shutil.copy2(OS_ELF, os.path.join(ISO, "boot", "os.elf"))

grub_cfg = """menuentry "ARC OS" {
    multiboot /boot/os.elf
    boot
}
"""
with open(os.path.join(ISO, "boot", "grub", "grub.cfg"), "w") as f:
    f.write(grub_cfg)

# Placeholder raw images (not bootable yet; kept for future disk support).
with open(os.path.join(ROOT, "os_floppy.img"), "wb") as f:
    f.write(b"\x00" * 1440 * 1024)
with open(os.path.join(ROOT, "os_harddisk.img"), "wb") as f:
    f.write(b"\x00" * 10 * 1024 * 1024)

print("ISO files created!")
print("  iso/boot/os.elf: %d bytes" % os.path.getsize(os.path.join(ISO, "boot", "os.elf")))
print("  os_floppy.img: %d bytes" % os.path.getsize(os.path.join(ROOT, "os_floppy.img")))
print("  os_harddisk.img: %d bytes" % os.path.getsize(os.path.join(ROOT, "os_harddisk.img")))
