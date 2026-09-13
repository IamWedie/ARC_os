@echo off
echo === ARC OS ISO Creator ===

mkdir iso\boot\grub 2>nul
copy os.elf iso\boot\ >nul 2>&1

echo menuentry "ARC OS" {
    multiboot /boot/os.elf
    boot
} > iso\boot\grub\grub.cfg

grub-mkrescue -o os.iso iso 2>nul
if exist os.iso (
    echo ISO created: os.iso
    echo Boot with: qemu-system-x86_64 -cdrom os.iso
) else (
    echo grub-mkrescue not available. Use direct kernel boot instead.
    echo qemu-system-x86_64 -kernel os.elf -nographic -m 512M
)
