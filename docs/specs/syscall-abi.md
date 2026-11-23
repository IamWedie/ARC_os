# ARC OS Syscall ABI (M4)

## Calling convention

- `int 0x80` from ring 3
- `rdi` = syscall number
- `rsi` = arg1, `rdx` = arg2, `rcx` = arg3
- return value in `rax` (negative on error)

## Stable syscalls (1-16, append-only)

| Nr | Name | Args | Returns | Notes |
|----:|------|------|---------|-------|
| 1 | `SYS_WRITE` | `(fd, buf, count)` | bytes written / -1 | fd 1/2 console |
| 2 | `SYS_READ` | `(fd, buf, count)` | bytes read / 0 / -1 | console stdin blocks at syscall boundary |
| 3 | `SYS_EXIT` | `(code)` | does not return | |
| 4 | `SYS_OPEN` | `(name, flags)` | fd / -1 | flags: `O_RDONLY=0,O_WRONLY=1,O_RDWR=2,O_CREAT=0x100,O_TRUNC=0x200,O_APPEND=0x400` |
| 5 | `SYS_CLOSE` | `(fd)` | 0 / -1 | |
| 6 | `SYS_LS` | `()` | 0 | prints to console |
| 7 | `SYS_DEL` | `(name)` | 0 / -1 | |
| 8 | `SYS_SEEK` | `(fd, offset, whence)` | new offset / -1 | whence 0/1/2 |
| 9 | `SYS_SIZE` | `(fd)` | size / -1 | |
| 10 | `SYS_TRUNC` | `(fd)` | 0 / -1 | |
| 11 | `SYS_RENAME` | `(old, new)` | 0 / -1 | |
| 12 | `SYS_CLEAR` | `()` | 0 | |
| 13 | `SYS_DUP` | `(fd)` | new fd / -1 | |
| 14 | `SYS_FORK` | `()` | child pid in parent, 0 in child, -1 | |
| 15 | `SYS_WAIT` | `(status_ptr)` | pid / -1 | |
| 16 | `SYS_EXEC` | `(path, argv)` | -1 on error, no return on success | |

## M4 syscalls (17+, append-only)

| Nr | Name | Args | Returns | Notes |
|----:|------|------|---------|-------|
| 17 | `SYS_STAT` | `(path, struct arc_stat*)` | 0 / -1 | |
| 18 | `SYS_READDIR` | `(path, index, struct arc_dirent*)` | 0 / -1 | index 0..N; -1 when done |
| 19 | `SYS_MKDIR` | `(path)` | 0 / -1 | FAT32 only; ARCFS returns -1 |
| 20 | `SYS_UNLINK` | `(path)` | 0 / -1 | |
| 21 | `SYS_MOUNT` | `(devname, fstype, target)` | 0 / -1 | `fstype` "fat32" |
| 22 | `SYS_SYNC` | `()` | 0 / -1 | Sync all mounted filesystems: FS sync hook then blockdev flush; returns -1 if any mount fails |
| 23 | `SYS_FSYNC` | `(fd)` | 0 / -1 | Validate fd, then sync mounted filesystems to stable media; propagates flush errors |

## Structures

```c
struct arc_stat {
    uint32_t size;
    uint32_t is_dir;
    uint32_t attrs;
    uint32_t reserved;
};

struct arc_dirent {        /* VFS_NAME_MAX = 64 */
    char name[64];
    uint32_t size;
    uint32_t is_dir;
};
```

## ABI rules

- Syscalls 1-16 are frozen: never renumbered or removed.
- New syscalls are appended from 17+.
- Struct layouts may be extended only in backward-compatible ways (append reserved/padding fields) or with new syscall numbers.