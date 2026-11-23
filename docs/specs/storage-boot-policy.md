# Storage Boot Policy (M4.4 E / E.1)

## Boot disk selection

Boot disk selection is deterministic. After AHCI and ATA block devices are registered:

1. Scan devices in order: `ahci0` .. `ahciN`, then `ata0`.
2. For each device, probe MBR/GPT partitions via `part_probe`.
3. For GPT disks, probe the FAT32 BPB of Microsoft Basic Data and EFI System partitions. **Never select an EFI System Partition (ESP) as root**, even if it has an ARCROOT marker/label.
4. A disk may be chosen as root only if its FAT32 volume label is `ARCROOT` (BPB volume label, space-padded) or it contains a file `/ARCROOT.MRK`.
5. The root mount is the first qualifying ARCROOT device/partition in scan order. Log the choice: `STORAGE: root = <dev> partition <n> (FAT32)`.
6. If no disk qualifies, keep today's behavior only for QEMU-prefixed model devices (model string starts with `QEMU`): select the first mountable FAT32 partition as before, including ATA whole-disk FAT32 fallback for ATA. This preserves existing QEMU gates.
7. If no disk qualifies and no QEMU fallback mounts, mount nothing and log: `STORAGE: no ARCROOT volume found`.
8. Never mount two devices as root. Other FAT32 partitions on other disks are not mounted automatically.

## AHCI write policy

AHCI block devices are registered for ports that are `identify_ok`, started, 512-byte sectors, and RW DMA EXT supported. Failed ports register nothing.

- A disk without FLUSH CACHE EXT support registers read-only (writes return error) because it cannot honor `docs/specs/blockdev-flush-durability.md`.
- A disk is writable only if:
  1. it is the chosen root disk,
  2. it passed IDENTIFY and flush support checks, and
  3. either its model string starts with `QEMU` or the build defines `ARC_AHCI_WRITE_REAL=1`.
- Otherwise it is registered read-only and logged: `AHCI: port N read-only (real hardware, ARC_AHCI_WRITE_REAL not set)`.
- Default builds do not define `ARC_AHCI_WRITE_REAL`.
- `AHCI_SELFTEST` writes only to the `ARCSCRATCH` disk, never to a disk selected as root.

## Read-only mount safety

When mounting a read-only block device, the FAT32 layer must not write anything (no dirty-bit updates, no FSInfo updates, no cleanup writes). Writes through FAT32/VFS return an error.

## Failure behavior

AHCI failures are non-fatal. A port that fails later makes its blockdev return errors immediately (never hang) and logs once. ATA PIO fallback remains intact.
