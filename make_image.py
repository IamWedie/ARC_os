import os, shutil, subprocess

base = 'C:/Users/wadia/os_project'

# Create directories for ISO
os.makedirs(f'{base}/iso/boot/grub', exist_ok=True)
os.makedirs(f'{base}/iso/boot/kernel', exist_ok=True)

# Copy ELF and BIN to ISO
shutil.copy2(f'{base}/os.elf', f'{base}/iso/boot/os.elf')
shutil.copy2(f'{base}/os.bin', f'{base}/iso/boot/os.bin')

# Create grub.cfg
grub_cfg = """menuentry "ARC OS" {
    multiboot /boot/os.elf
    boot
}
"""
with open(f'{base}/iso/boot/grub/grub.cfg', 'w') as f:
    f.write(grub_cfg)

# Create bootable floppy disk image (1.44MB)
with open(f'{base}/os_floppy.img', 'wb') as f:
    f.write(b'\x00' * 1440 * 1024)

# Create hard disk image (10MB)
with open(f'{base}/os_harddisk.img', 'wb') as f:
    f.write(b'\x00' * 10 * 1024 * 1024)

print('ISO files created!')
print(f'  iso/boot/os.elf: {os.path.getsize(f"{base}/iso/boot/os.elf")} bytes')
print(f'  iso/boot/os.bin: {os.path.getsize(f"{base}/iso/boot/os.bin")} bytes')
print(f'  os_floppy.img: {os.path.getsize(f"{base}/os_floppy.img")} bytes')
print(f'  os_harddisk.img: {os.path.getsize(f"{base}/os_harddisk.img")} bytes')
print(f'  grub.cfg created')
