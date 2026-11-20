# Design: Per-Process POSIX Descriptor Layer

**Date:** 2026-11-20
**Status:** Draft for human approval
**Owner:** ARC OS kernel/POSIX layer

## Intent and the why

ARC OS currently exposes file descriptors that are just `FS_SLOT_FIRST + slot`.
The read/write position lives on the shared `fs_file_t`, so two opens of the
same file share one cursor. `fs_close()` only resets that cursor; it does not
close a descriptor. There is no per-process descriptor table, no access-mode
checking, and no close-on-exit.

This is acceptable for a one-process demo but blocks the stated goal of a
real POSIX-compatible OS. `fork` must give the child its own descriptor table
while preserving shared open file descriptions where POSIX requires it. `dup`
must share an offset. Deleting an open file must not leave a descriptor pointing
at blocks that have already been recycled.

The goal of this slice is to establish that model **before** `fork`/`exec`
exists, so later work inherits correct semantics instead of rewriting them.

## Goals

1. Descriptor numbers are per-process and validated against that process's
   table. FD 0/1/2 are standard input/output/error for every process.
2. Each `open()` creates an **open file description (OFD)** holding file slot,
   offset, flags, and a refcount. Fds point to OFDs.
3. `dup()` creates another fd in the same process that points to the same OFD,
   so both share one offset. `fork` can later clone the fd table by incrementing
   OFD refs.
4. `close()` releases one fd; the OFD and its file-slot reference are released
   when the last fd referring to it closes.
5. `exit`/reap closes all descriptors owned by the process.
6. Access flags (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`,
   `O_APPEND`) are checked at the syscall boundary.
7. Deleting an open file defers freeing its blocks until the last descriptor
   closes, so descriptors never point at recycled storage.
8. The descriptor module is host-testable with the real implementation and
   small stubs, following the existing `tests/pmm_stub.c` pattern.

## Non-goals

- FAT32 or a general VFS. The backend remains the current RAM/disk-backed
  block filesystem.
- `fork`, `exec`, and a real `wait()` — this slice only makes their future
  fd behaviour correct.
- `dup2`, `fcntl`, non-blocking I/O, pipes, sockets, directory fds.
- Per-process working directory or permission checks.
- Changing the boot image or scheduler policy.

## Constraints

- Freestanding C11, Zig 0.16 cross-build, no SSE/MMX in kernel code.
- `kernel/fs.c` currently persists `fs_file_t` directly to disk; adding fields
  requires bumping `FS_DISK_VERSION` to 2 and reformatting older images.
- The ring-3 shell passes `create` as syscall argument 2 today. The ABI change
  to flags requires updating every `SYS_OPEN` call in `userspace/shell.c`.
- Host tests must link real kernel logic with stubs, as the slab/scheduler
  suites already do.
- Quality gate is `cmd /c tests\run_all.bat`: host tests → build → TCP-serial
  QEMU shell session.

## Current state (what this change touches)

- `kernel/kernel.h` — `process_t`, syscall numbers, `fs_file_t`.
- `kernel/fs.c` — slot lookup, extent allocation, file metadata.
- `kernel/syscall.c` — currently routes fd ≥ 3 directly to `fs_*`.
- `kernel/scheduler.c` / `kernel/user.c` — process creation point where std fds
  must be installed; reaping point where fds must be closed.
- `userspace/shell.c` — all `SYS_OPEN` call sites.
- `tests/` — new fd suite and runner steps.

## Approaches considered

### A. Global descriptor table

Keep one descriptor table for the whole kernel and add independent offsets and
refcounts there.

- **Trade-off:** smallest diff, fixes the shared-offset bug today.
- **Problem:** `fork` cannot give parent and child distinct descriptor tables.
  We would have to add per-process ownership later, changing the ABI and every
  descriptor reference again.

### B. Per-process fds with offsets stored on each fd

Give every `process_t` an fd array and store `file_slot + offset + flags` in
each entry.

- **Trade-off:** fixes today's bug and closes fds on exit.
- **Problem:** `dup()` and `fork()` should share one open file description and
  therefore one offset. This model would either make them wrong or require a
  structural rewrite later.

### C. Per-process fds pointing to shared open file descriptions

Fds are per-process. Each `open()` creates an OFD with its own offset and
refcount. `dup()` and `fork` reuse the OFD; `close()` drops one reference.

- **Trade-off:** one additional fixed-size table and an explicit refcount
  discipline.
- **Benefit:** matches POSIX, makes `dup`/`fork` correct by construction, and
  gives deferred unlink a natural home.
- **Decision:** chosen. The extra structure is small (~512 B for 32 OFDs) and
  directly serves the multi-year goal.

## Architecture

### New public types (in `kernel/kernel.h`)

```c
#define FD_MAX 16
#define OFD_MAX 32

#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_ACCMODE 0x0003
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

typedef enum {
    FD_ENTRY_FREE = 0,
    FD_ENTRY_CONSOLE_IN,
    FD_ENTRY_CONSOLE_OUT,
    FD_ENTRY_CONSOLE_ERR,
    FD_ENTRY_FILE
} fd_entry_kind_t;

typedef struct {
    int used;
    int file_slot;
    uint32_t offset;
    uint32_t flags;
    unsigned refs;
} open_file_t;

typedef struct {
    fd_entry_kind_t kind;
    int ofd;          /* index into the global OFD table, or -1 */
    uint32_t flags;   /* per-fd flags; currently close-on-exec is reserved */
} fd_entry_t;
```

`process_t` gains `fd_entry_t fds[FD_MAX]`. The global OFD pool is
`open_file_t g_ofds[OFD_MAX]` in `kernel/fd.c`.

### fd API (`kernel/fd.c`)

```c
void fd_init_process(process_t* p);
void fd_close_all(process_t* p);
int fd_clone_process(const process_t* parent, process_t* child); /* for fork */

int fd_open(int pid, const char* name, uint32_t flags);
int fd_dup(int pid, int oldfd);
int fd_close(int pid, int fd);
int fd_read(int pid, int fd, void* buf, size_t count);
int fd_write(int pid, int fd, const void* buf, size_t count);
int fd_seek(int pid, int fd, int64_t offset, int whence);
int fd_size(int pid, int fd);
int fd_truncate(int pid, int fd);
```

`whence` supports `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`; `offset` is signed so
`SEEK_CUR` can move backwards. The resulting offset must land in `[0, size]`.
Seeking past EOF is rejected in this slice because the block allocator has no
sparse-file support. Invalid pids, fds, flags, or access modes return -1. No
function allocates dynamically: fd and OFD pools are fixed size.

### Filesystem slot API (`kernel/fs.c`)

`fd.c` uses slot-level operations, not descriptor numbers:

```c
int fs_open_slot(const char* name, int create);
int fs_slot_read(int slot, uint32_t off, void* buf, size_t count);
int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count);
int fs_slot_size(int slot);
int fs_slot_truncate(int slot);
int fs_slot_add_ref(int slot);
void fs_slot_release(int slot);
```

`fs_file_t` gains `uint16_t open_refs` and `uint8_t deleted`. `fs_delete(name)`
marks `deleted = 1` and frees the extent only when `open_refs == 0`.
`fs_slot_release()` frees the extent when the last reference goes away on a
deleted file. `FS_DISK_VERSION` becomes 2; older images are reformatted.

The old fd-shaped `fs_open`/`fs_read`/`fs_write`/`fs_close`/`fs_seek`/
`fs_file_size`/`fs_truncate` functions are removed once `syscall.c` and the
shell no longer call them. Keeping both would create two competing fd
namespaces.

### Syscall boundary (`kernel/syscall.c`)

`SYS_OPEN` argument 2 becomes flags. `SYS_SEEK` gains `whence` in argument 3.
A new `SYS_DUP = 13` is added. `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE`,
`SYS_SIZE`, and `SYS_TRUNC` call the fd layer for fd ≥ 3. Std descriptors are
handled inside `fd.c` so every process gets the same behaviour.

### Process lifecycle integration

- `process_build_common()` calls `fd_init_process()` so fd 0/1/2 exist before
  the process is queued.
- `scheduler_reap()` calls `fd_close_all()` before clearing the slot. This
  composes with the existing automatic idle reaper.
- `fd_clone_process()` is added now but has no production caller until `fork`.

### Shell ABI updates

All `SYS_OPEN` call sites in `userspace/shell.c` move to explicit flags:

| Command | Flags |
|---------|-------|
| `cat` / read paths | `O_RDONLY` |
| `touch` | `O_WRONLY \| O_CREAT` |
| `truncate` | `O_WRONLY` |
| `echo text to file` | `O_WRONLY \| O_CREAT \| O_TRUNC` |
| `cp` source | `O_RDONLY` |
| `cp` destination | `O_WRONLY \| O_CREAT \| O_TRUNC` |

`echo text to file` becomes overwrite semantics, replacing the old implicit
append. This is an intentional POSIX-like change; call it out in the final
commit and docs.

## Testing strategy

New `tests/test_fd.c` links the real `kernel/fd.c` with `tests/fd_stub.c`
providing fake fs slots and console I/O. Cases:

1. Std fds route to the console backend and reject invalid access.
2. Two separate `open()` calls on one file have independent offsets.
3. `dup()` shares the offset; closing the original leaves the duplicate valid.
4. Closing the last fd for an OFD releases the slot reference.
5. Two processes have independent fd tables; reaping one closes only its fds.
6. `fd_clone_process()` shares OFDs and increments refs.
7. `O_WRONLY` reads fail; `O_RDONLY` writes fail; `O_CREAT`/`O_TRUNC` behave as
   specified.
8. `SEEK_SET`/`CUR`/`END` bounds are checked.
9. Deleting a file while open keeps reads working until the last close.
10. Exhausting fd/OFD pools fails cleanly with no leaked refs.

`tests/run_tests.bat` gains a build+run step for this suite. The full
`cmd /c tests\run_all.bat` gate must remain green, including the existing
shell commands.

## Risks and mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| ABI flag change breaks the shell | High | Update every call site; boot test covers echo/touch/cat/cp paths |
| Refcount bug leaks or double-frees a slot | High | Host tests assert refcounts and net-zero refs after close-all |
| Disk layout change loses existing files | Medium | Bump `FS_DISK_VERSION` and reformat; document as a format break |
| Fixed OFD pool exhausted | Low | Return -1 cleanly; 32 OFDs exceed current `FD_MAX` × 1 process; revisit when fork lands |
| Deferred unlink complexity grows | Medium | Keep it to one `deleted` flag and one `open_refs` counter; no directory entries involved |

## Resolved design calls (post builder check)

A fresh reviewer raised seven questions that would otherwise force a guess.
These are settled here as implementation calls; they are not open questions for
the human partner.

1. **Private fs helpers vs. public API.** `fs_init()` seeding and
   `fs_selftest()` may keep private static helpers inside `kernel/fs.c`. Only
   the public fd-shaped fs functions are removed; callers outside `fs.c` use
   the slot API. This avoids rewriting boot-time setup.
2. **Mount handling for transient fields.** `open_refs` and `deleted` are
   persisted with the directory table for format simplicity but treated as
   transient. `fs_init()` clears `open_refs` on load; any entry still marked
   `deleted` has its extent freed and the slot marked unused. This prevents
   stale deleted-open blocks from surviving a reboot.
3. **Standard fd contract.** Fd 0/1/2 are ordinary entries initially pointing
   to console-in/out/err. They may be closed and their numbers reused by a
   later `open()`. `fd_seek`, `fd_size`, and `fd_truncate` fail on console
   entries. `fd_dup()` of a console fd produces another console entry of the
   same direction. Stderr uses the same terminal backend as stdout; stdin
   reads use `kbd_getchar()` and stop at a newline for fd 0.
4. **Deleted file names.** A deleted-but-open slot keeps its name internally,
   but `fs_find_slot()`, `fs_list()`, and `fs_rename()` skip `deleted` entries.
   A later `open(name, O_CREAT)` can create a new slot with the same name while
   old readers still hold the deleted one.
5. **`O_APPEND` is functional.** On every write with `O_APPEND`, the fd layer
   asks the slot for its current size and writes there, ignoring the fd's
   stored offset. Interleaving writers are serialized by the UP kernel's
   non-preemptible syscall path; SMP will need an atomic append later.
6. **Seek bounds.** `fd_seek()` takes `int64_t` and supports negative
   `SEEK_CUR`. The result must be within `[0, size]`; `SEEK_END` with a
   negative offset is allowed. Seeking past EOF is a documented limitation of
   this slice.
7. **PID 0 fd table.** `scheduler_init()` calls `fd_init_process(&processes[0])`
   in addition to `process_build_common()`, so the kernel/idle context has a
   valid std-fd table even before the first user process exists.

## What is left to the builder

The implementation order, exact file-by-file task breakdown, and verification
checkpoints are intentionally left to the planning step after this design is
approved. The builder should not invent new ABI numbers or resource limits
without updating this document first.

## Approval

This design is not approved until the human partner explicitly accepts it.
After approval, the next step is the written implementation plan, then RED/GREEN
implementation.
