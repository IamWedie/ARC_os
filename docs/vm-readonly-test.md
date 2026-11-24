# VM read-only AHCI test (manual)

This procedure boots ARC OS from `os.iso` in a BIOS-mode VirtualBox VM with a
small ARCROOT-labelled FAT32 disk attached to an Intel AHCI SATA controller.
The disk is read-only because this build does not define `ARC_AHCI_WRITE_REAL`.
No physical disk is touched: the disk image is a regular file created by
`tools/make_arcroot_disk.py`.

## 1. Prerequisites

- Build `os.elf` and create `os.iso` with GRUB (`grub-mkrescue -o os.iso iso`).
- Have `VBoxManage` installed.
- Use a scratch directory, for example `C:\Users\wadia\ARC_os\vm-test`.

## 2. Create the ARCROOT disk image

From the repo root:

```cmd
python tools\make_arcroot_disk.py vm-test\arcroot.img
Get-FileHash -Algorithm SHA256 vm-test\arcroot.img
```

Record the SHA256 before boot. The image is 16 MiB, MBR, one FAT32 partition
(type `0x0C`), volume label `ARCROOT`, and contains `welcome.txt`.

Convert it to a VDI container for VirtualBox:

```cmd
VBoxManage convertfromraw vm-test\arcroot.img vm-test\arcroot.vdi --format VDI
```

The guest must not write to the disk. Compare the hash of the **source raw
image** after the test; VDI container metadata may be updated by VirtualBox on
open and is not the guest-visible payload.

## 3. Configure a BIOS-mode VM with SATA and serial logging

```cmd
VBoxManage createvm --name ARC-ReadOnly --register
VBoxManage modifyvm ARC-ReadOnly --firmware bios --boot1 dvd --boot2 disk --boot3 none --boot4 none
VBoxManage modifyvm ARC-ReadOnly --memory 512 --acpi on --ioapic on --nic1 none
VBoxManage modifyvm ARC-ReadOnly --uart1 0x3F8 4 --uartmode1 file C:\Users\wadia\ARC_os\vm-test\arc-serial.log
VBoxManage storagectl ARC-ReadOnly --name SATA --add sata --controller IntelAhci --bootable on
VBoxManage storageattach ARC-ReadOnly --storagectl SATA --port 0 --device 0 --type hdd --medium C:\Users\wadia\ARC_os\vm-test\arcroot.vdi
VBoxManage storagectl ARC-ReadOnly --name IDE --add ide --bootable on
VBoxManage storageattach ARC-ReadOnly --storagectl IDE --port 0 --device 0 --type dvddrive --medium C:\Users\wadia\ARC_os\os.iso
```

Firmware must be BIOS/CSM, not UEFI. The serial port is COM1 (`0x3F8`, IRQ 4)
and its output goes to the raw file `vm-test\arc-serial.log`.

## 4. Boot and exercise the disk

```cmd
VBoxManage startvm ARC-ReadOnly --type headless
```

After the shell prompt appears in `vm-test\arc-serial.log`, send these commands
through the VM serial console (or run interactively in the VM window):

```
ls
echo hello > ro.txt
sync
```

Power off the VM cleanly, then compare the image hash:

```cmd
Get-FileHash -Algorithm SHA256 vm-test\arcroot.img
```

## 5. Pass criteria

The serial log must show all of the following:

- `AHCI: version ... ports=... pi=... s64a=...` capability line
- `AHCI: port N sig=... sata started`
- `AHCI: port N model="..." sectors=... lba48=... ss=... flush=...` for a
  non-QEMU model (for example `VBOX-LIKE-DISK`)
- `STORAGE: root = ahci0 partition 1 (FAT32)`
- `AHCI: port N read-only (real hardware, ARC_AHCI_WRITE_REAL not set)`
- `ls` lists `welcome.txt` with size 74 bytes
- `echo hello > ro.txt` fails cleanly with `echo: cannot create ro.txt`
- `sync` returns `sync: 0`
- no `PANIC` or `EXCEPTION`

The SHA256 of `vm-test\arcroot.img` must be identical before and after boot.

## 6. QEMU equivalent (no VirtualBox required)

This is the same policy path using a QEMU model string that is not
QEMU-prefixed:

```cmd
qemu-system-x86_64 -kernel os.elf -m 512M -display none -vga none -device bochs-display,vgamem=67108864 -device ich9-ahci,id=ahci -drive file=vm-test\arcroot.img,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ahci.0,model=VBOX-LIKE-DISK -serial stdio -monitor none -no-reboot -no-shutdown -accel tcg
```

Expected serial output is the same as the VirtualBox pass criteria. This QEMU
run uses `-kernel os.elf` because building `os.iso` requires GRUB tooling; it
exercises the same AHCI/FAT32/storage-policy code.
