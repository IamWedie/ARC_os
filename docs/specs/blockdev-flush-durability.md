# Block Device Flush Durability Contract (M4.10a)

## Contract

`int blockdev_flush(struct blockdev* dev)` returns `0` only when all previously
completed writes to `dev` have reached stable media. If the device has a
write-back cache, the driver must issue a device cache flush command and wait
for completion before returning success.

`blockdev_flush` returns `-1` on error, including device error status, device
fault, timeout, or missing driver support where flush is required but cannot be
performed.

## ATA PIO

The ATA PIO driver implements the contract with:

- `FLUSH CACHE` (`0xE7`) for non-LBA48 devices.
- `FLUSH CACHE EXT` (`0xEA`) when IDENTIFY indicates LBA48 support (word 83 bit 10).

The command is issued to the selected primary master device and polled until BSY
clears or the driver timeout expires. If the status contains ERR/DF, or the wait
times out, `ata_flush_cache()` returns `-1`.

## VFS / FS sync

`int vfs_sync(const char* mount)` synchronizes a mounted filesystem:

1. Call the mounted filesystem's `sync` hook when present. The hook writes any
   dirty cached filesystem metadata back through the block device.
2. Call `blockdev_flush()` on the mount's backing block device.

`vfs_sync` returns `0` only when both steps succeed. Errors are propagated.

`int vfs_sync_all(void)` applies `vfs_sync()` to every active mount and returns
`-1` if any mount sync fails.

## FAT32

FAT32 currently uses write-through block device writes for file data and
metadata updates. Its `sync` hook reports success when the filesystem is mounted
and has no dirty cached metadata to write back; `vfs_sync()` then issues the
required `blockdev_flush()` to satisfy the stable-media durability boundary.
