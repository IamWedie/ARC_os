# Hardware Log

Record real-hardware boot results here (M3 onward, per roadmap section 2 rule 5 and section 8).

| Date | Milestone | Machine | CPU | RAM | Firmware (UEFI/Legacy) | Secure Boot | Boot media | Result | Notes / fixes |
|------|-----------|---------|-----|-----|------------------------|-------------|------------|--------|---------------|
| 2026-11-22 | M3 | N/A (no physical hardware in CI/dev env) | N/A | N/A | N/A | N/A | N/A | DEFERRED | QEMU gates pass (gui/fork/exec/mb2). Real-hardware boot not executed in this environment; run checklist below on target PC. |
| 2026-11-23 | M4 | N/A (no physical hardware in CI/dev env) | N/A | N/A | N/A | N/A | N/A | DEFERRED | QEMU persistence gate passes (FAT32 mount/create/reboot/read-back). AHCI/real-disk hardware not executed in this environment; run M4 checklist on target PC. |

## M3 Real-Hardware Checklist

**Preconditions**
- [ ] Build `os.elf`: `python tools/build_kernel.py --quiet`
- [ ] Create boot image: `python tools/make_usb_image.py` (produces `build/arc-os.iso` and `build/arc-os-usb.img`)
- [ ] Flash USB: Rufus DD mode for `.img` (or ISO mode), or Linux `dd if=build/arc-os-usb.img of=/dev/sdX bs=4M conv=fsync status=progress`
- [ ] Target PC: x86_64, disable Secure Boot, enable CSM/Legacy boot if needed for GRUB BIOS image

**Boot checks**
- [ ] GRUB menu shows "ARC OS" and boots kernel via Multiboot2
- [ ] On-screen early debug log appears after graphics init (top-left panel)
- [ ] Desktop renders on firmware framebuffer (`boot_fb` path) or fallback path logs clearly
- [ ] PS/2 mouse moves cursor (if present)
- [ ] Keyboard works in shell
- [ ] No panic/exception; if panic occurs, on-screen panic screen shows registers and stack trace

**Record**
- [ ] Capture photo/serial log if available
- [ ] Add row to table above with machine/firmware/result/notes

## M4 Real-Hardware Checklist

**Preconditions**
- [ ] Build `os.elf` and create boot media as in M3
- [ ] Attach a SATA/AHCI disk with a FAT32 partition (or blank disk for image install)
- [ ] Disable Secure Boot; prefer AHCI mode in firmware

**Boot checks**
- [ ] PCI enumerates storage controller; AHCI/ATA presence logged on serial
- [ ] FAT32 partition mounts or clear fallback/error is logged
- [ ] `echo hello > persist.txt`, reboot, `cat persist.txt` returns `hello`
- [ ] `execdemo` works when `hello.bin` is present on disk
- [ ] No data corruption after clean reboot

**Record**
- [ ] Capture serial log/photo
- [ ] Add row to table above with machine/firmware/storage controller/result/notes
