# POSIX FD Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace slot-index pseudo-fds with per-process descriptor tables that point to shared, refcounted open file descriptions, including access flags, independent offsets, `dup`, close-on-exit, and deferred unlink.

**Architecture:** `kernel/fd.c` owns fd tables and the OFD pool and talks to the filesystem only through slot-level operations. `kernel/fs.c` exposes those slot operations, tracks open refs and deleted-but-open state, and keeps private helpers for boot-time seeding/selftest. `kernel/syscall.c` and the ring-3 shell move to the new flag-based ABI.

**Tech Stack:** Freestanding C11, Zig 0.16 kernel cross-build, native GCC host tests, QEMU TCP-serial runtime test.

**Spec:** `docs/specs/posix-fd-layer-design.md`

## Global Constraints

- Use Zig `C:\Users\wadia\zig\ziglang\zig.exe` (0.16.0) and host GCC `C:\msys64\ucrt64\bin\gcc.exe`.
- Kernel builds with `-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-omit-frame-pointer -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-red-zone -O2`; the build rejects vector instructions.
- Use `cmd /c tests\run_tests.bat` for host suites and `cmd /c tests\run_all.bat` for the full gate.
- Fixed limits: `FD_MAX 16`, `OFD_MAX 32`, `MAX_PROCESSES 16`.
- Flags are exactly `O_RDONLY 0x0000`, `O_WRONLY 0x0001`, `O_RDWR 0x0002`, `O_ACCMODE 0x0003`, `O_CREAT 0x0100`, `O_TRUNC 0x0200`, `O_APPEND 0x0400`.
- Syscall numbers are unchanged except new `SYS_DUP 13`; `SYS_SEEK` argument 3 becomes `whence`.
- `FS_DISK_VERSION` becomes 2; older images are reformatted on mount.
- Deleted-but-open blocks are freed only when the last ref closes or on mount normalization.
- No dynamic allocation in `kernel/fd.c`; no change to scheduler policy or MLFQ counters.
- Commit every task after its own verification passes.

## Review Focus

- Two independent `open()` calls on one file must not share an offset → `test_independent_offsets` in Task 2.
- `dup()` and a cloned process must share one OFD offset and refcount → `test_dup_shares_offset` and `test_clone_process` in Task 2.
- Closing fd 1 must free the number so a later `open()` can reuse it for a file → `test_close_std_fd_reuses_number` in Task 2.
- Deleting a file while open must keep old reads valid and free blocks at last close → `test_slot_refcount_and_deferred_unlink` in Task 3.
- `O_CREAT` after a deleted-but-open file must create a new visible file without disturbing the old reader → `test_hidden_deleted_name_reuse` in Task 3.

---

### Task 1: Std descriptors and fd host harness

**Files:**
- Modify: `kernel/kernel.h`
- Create: `kernel/fd.c`
- Create: `tests/fd_stub.c`
- Create: `tests/test_fd.c`
- Modify: `tests/run_tests.bat`

**Interfaces:**
- Consumes: `process_t`, `terminal_write()`, `kbd_getchar()`, `processes[]`, `current_process`.
- Produces:
  - Constants `FD_MAX`, `OFD_MAX`, `FD_STDIN/STDOUT/STDERR`, `O_*`, `SEEK_*`, `SYS_DUP`.
  - Types `fd_entry_kind_t`, `fd_entry_t`, `open_file_t`; `process_t.fds[FD_MAX]`.
  - `void fd_init_process(process_t* p);`
  - `int fd_close(int pid, int fd);`
  - `int fd_read(int pid, int fd, void* buf, size_t count);`
  - `int fd_write(int pid, int fd, const void* buf, size_t count);`
  - `int fd_seek(int pid, int fd, int64_t offset, int whence);`
  - `int fd_size(int pid, int fd);`
  - `int fd_truncate(int pid, int fd);`
  - Declarations only, implemented in Task 2: `fd_open`, `fd_dup`, `fd_clone_process`, `fd_close_all`.

- [ ] **Step 1: Add ABI declarations and a failing std-fd test**

In `kernel/kernel.h`, add the constants, types, `process_t.fds`, and prototypes listed above. In `tests/test_fd.c`, write `test_std_fds()` with these assertions:
  - `fd_init_process(&processes[1])` leaves fd 0/1/2 as console-in/out/err.
  - `fd_write(1, FD_STDOUT, "hi", 2) == 2` and the stub console captured `"hi"`.
  - `fd_write(1, FD_STDERR, "e", 1) == 1` and the stub console captured `"e"`.
  - `fd_read(1, FD_STDIN, buf, 1) == 1` and `buf[0] == 'a'` from the stub console.
  - `fd_seek(1, FD_STDOUT, 0, SEEK_SET) == -1`.
  - `fd_size(1, FD_STDOUT) == -1` and `fd_truncate(1, FD_STDOUT) == -1`.
  - `fd_close(1, FD_STDOUT) == 0` and a later `fd_write(1, FD_STDOUT, "x", 1) == -1`.

In `tests/fd_stub.c`, define `process_t processes[MAX_PROCESSES]`, `volatile int current_process`, and console stubs that record writes and return `a\n` on reads.

Run:
```cmd
C:\msys64\ucrt64\bin\gcc.exe -O2 -Wall -Wextra -std=gnu11 -I kernel tests\test_fd.c tests\fd_stub.c kernel\fd.c -o tests\test_fd.exe
tests\test_fd.exe
```
Expected: FAIL at compile/link time because `kernel/fd.c` and the `fd_*` definitions do not exist yet.

- [ ] **Step 2: Implement std fd lifecycle**

Create `kernel/fd.c` with the fd/OFD structs, `fd_init_process()`, and console-backed `fd_read`/`fd_write`. Implement `fd_close()`, `fd_seek()`, `fd_size()`, and `fd_truncate()` for console entries: close clears the entry; seek/size/truncate return -1. Leave file-backed paths unimplemented until Task 2.

Run the focused command from Step 1.
Expected: PASS with all `test_std_fds()` checks green.

- [ ] **Step 3: Add the suite to the host runner and verify all host tests**

Add a fifth build/run step to `tests/run_tests.bat` for `test_fd.c` + `fd_stub.c` + `kernel/fd.c`. Run:
```cmd
cmd /c tests\run_tests.bat
```
Expected: slab 2269 checks, scheduler 343 checks, fd suite green, 0 failures.

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel.h kernel/fd.c tests/fd_stub.c tests/test_fd.c tests/run_tests.bat
git commit -m "feat: add per-process std descriptor table"
```

---

### Task 2: Open file descriptions over stub slots

**Files:**
- Modify: `kernel/fd.c`
- Modify: `tests/fd_stub.c`
- Modify: `tests/test_fd.c`

**Interfaces:**
- Consumes: Task 1 fd types and std-fd behaviour; `fs_slot_*` declarations from `kernel/kernel.h`.
- Produces:
  - `int fd_open(int pid, const char* name, uint32_t flags);`
  - `int fd_dup(int pid, int oldfd);`
  - `void fd_close_all(process_t* p);`
  - `int fd_clone_process(const process_t* parent, process_t* child);`
  - File-backed behaviour for `fd_read`, `fd_write`, `fd_seek`, `fd_size`, `fd_truncate`, and `fd_close`.
  - Stub slot API in `tests/fd_stub.c`: `fs_open_slot`, `fs_slot_read`, `fs_slot_write`, `fs_slot_size`, `fs_slot_truncate`, `fs_slot_add_ref`, `fs_slot_release`.

- [ ] **Step 1: Write failing OFD tests**

Add to `tests/test_fd.c`:
  - `test_independent_offsets()`: open `"shared"` twice with `O_RDWR|O_CREAT`; write via fd A, then assert fd B still reads from offset 0.
  - `test_dup_shares_offset()`: `fd_dup()` fd A; write via the duplicate; assert the original's next read starts after the duplicate's write.
  - `test_close_std_fd_reuses_number()`: close fd 1, `fd_open()` a file, assert the returned fd is 1 and `fd_write(1, 1, ...)` goes to the stub file, not the console.
  - `test_access_flags()`: `O_WRONLY` read fails, `O_RDONLY` write fails, `O_CREAT` creates, `O_TRUNC` zeroes size.
  - `test_seek_bounds()`: `SEEK_SET`, `SEEK_CUR` with a negative delta, `SEEK_END` with a negative delta all succeed within the file; past EOF and before start return -1.
  - `test_clone_process()`: clone a process with one open fd; assert both process tables point to the same OFD and `fs_slot_add_ref` was called once on clone.
  - `test_pool_exhaustion()`: exhaust fd and OFD pools; assert -1 with no leaked slot refs.

Run focused build/run as in Task 1.
Expected: FAIL with undefined `fd_open`/`fd_dup`/slot functions.

- [ ] **Step 2: Implement the OFD pool and file-backed paths**

In `kernel/fd.c`, add `g_ofds[OFD_MAX]` and implement:
  - `fd_open` allocates an OFD (`file_slot`, `offset = 0`, `flags`, `refs = 1`) and the lowest free fd.
  - `fd_dup` copies the fd entry and increments the OFD refcount.
  - `fd_close` clears the fd entry and releases the OFD when `refs` reaches 0; call `fs_slot_release` once at that point.
  - `fd_read`/`fd_write` use `fs_slot_read`/`fs_slot_write` at `ofd->offset`, update the offset by bytes transferred, reject wrong access modes, and implement `O_APPEND` by asking `fs_slot_size()` on every write.
  - `fd_seek` computes `int64_t` targets from `SEEK_SET/CUR/END`, rejects outside `[0, size]`, and updates the OFD offset.
  - `fd_size`/`fd_truncate` delegate to the slot API.
  - `fd_close_all` and `fd_clone_process` follow the refcount rules in the design.

Extend `tests/fd_stub.c` fake slots with data, size, refs, and deleted state.

Run the focused command.
Expected: PASS with all seven new tests green.

- [ ] **Step 3: Run all host tests**

Run: `cmd /c tests\run_tests.bat`
Expected: all suites green, 0 failures.

- [ ] **Step 4: Commit**

```bash
git add kernel/fd.c tests/fd_stub.c tests/test_fd.c
git commit -m "feat: add shared open file descriptions"
```

---

### Task 3: Real filesystem slot backend

**Files:**
- Modify: `kernel/fs.c`
- Modify: `kernel/kernel.h`
- Create: `tests/test_fs_slot.c`
- Create: `tests/fs_stub.c`
- Modify: `tests/run_tests.bat`

**Interfaces:**
- Consumes: `fs_file_t`, `FS_MAX_FILES`, `FS_BLOCK_SIZE`, Task 2 slot API contract.
- Produces:
  - `int fs_open_slot(const char* name, int create);`
  - `int fs_slot_read(int slot, uint32_t off, void* buf, size_t count);`
  - `int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count);`
  - `int fs_slot_size(int slot);`
  - `int fs_slot_truncate(int slot);`
  - `int fs_slot_add_ref(int slot);`
  - `void fs_slot_release(int slot);`
  - `fs_file_t.open_refs` and `fs_file_t.deleted`; `FS_DISK_VERSION 2`.
  - Old fd-shaped fs functions remain temporarily as private-to-public wrappers for boot/selftest compatibility.

- [ ] **Step 1: Write failing real-slot tests**

In `tests/test_fs_slot.c`, write:
  - `test_slot_refcount_and_deferred_unlink()`: mount the fake fs, open a slot, add two refs, call `fs_delete("victim")`, assert reads still work, release one ref and assert data remains, release the last and assert the extent is free.
  - `test_hidden_deleted_name_reuse()`: after deleting while open, assert `fs_find_slot("victim")` no longer resolves it, `fs_open_slot("victim", 1)` creates a different slot, and the original reader still sees the old data.
  - `test_mount_normalizes_deleted_open()`: load a fake disk image containing a `deleted=1` entry with stale refs; assert `fs_init()` frees its extent and marks the slot unused.

In `tests/fs_stub.c`, provide minimal `kmalloc`/`kfree`, `ata_*`, and `terminal_*` stubs so real `kernel/fs.c` can run on the host. Physical frames come from the existing `tests/pmm_stub.c`.

Run:
```cmd
C:\msys64\ucrt64\bin\gcc.exe -O2 -Wall -Wextra -std=gnu11 -I kernel tests\test_fs_slot.c tests\fs_stub.c tests\pmm_stub.c kernel\fs.c -o tests\test_fs_slot.exe
tests\test_fs_slot.exe
```
Expected: FAIL with undefined slot functions and missing `fs_file_t` fields.

- [ ] **Step 2: Implement slot API and mount normalization**

In `kernel/fs.c`:
  - Add `open_refs`/`deleted` to `fs_file_t` and bump `FS_DISK_VERSION` to 2.
  - Implement the seven slot functions; `fs_slot_write` writes at an explicit offset and extends the extent when needed.
  - Make `fs_delete` mark `deleted`, hide the name from lookup/list/rename, and free the extent only when `open_refs == 0`.
  - Make `fs_slot_release` free a deleted slot's extent when refs reach 0.
  - Normalize stale deleted entries at mount.
  - Keep old fd-shaped functions as wrappers until Task 4 removes them.

Run the Step 1 command.
Expected: PASS with all three slot tests green.

- [ ] **Step 3: Run all host tests**

Add a `test_fs_slot.exe` step to `tests/run_tests.bat`, then run:
```cmd
cmd /c tests\run_tests.bat
```
Expected: all suites green, 0 failures.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs.c kernel/kernel.h tests/test_fs_slot.c tests/fs_stub.c tests/run_tests.bat
git commit -m "feat: add filesystem slot API with deferred unlink"
```

---

### Task 4: Runtime overwrite assertion

**Files:**
- Modify: `tools/boot_test.py`
- Modify: `tests/run_all.bat`

**Interfaces:**
- Consumes: existing TCP-serial boot harness.
- Produces: `--expect-not TEXT` (repeatable) that fails the boot test if TEXT appears anywhere in the captured serial output.

- [ ] **Step 1: Add the negative expectation and a failing roundtrip test**

Add `--expect-not TEXT` support to `tools/boot_test.py`. In `tests/run_all.bat`, add these shell inputs to the existing boot test:
  - `echo alpha to fdtest.txt`
  - `echo beta to fdtest.txt`
  - `cat fdtest.txt`
  - `rm fdtest.txt`
  and these expectations:
  - `--expect "beta"`
  - `--expect-not "alpha"`

Run:
```cmd
cmd /c tests\run_all.bat
```
Expected: FAIL at the boot-test stage because the current shell appends, so `alpha` still appears in `cat` output.

- [ ] **Step 2: Commit the test harness change**

```bash
git add tools/boot_test.py tests/run_all.bat
git commit -m "test: assert echo overwrite semantics in boot gate"
```

---

### Task 5: Wire kernel and shell to the fd layer

**Files:**
- Modify: `kernel/syscall.c`
- Modify: `kernel/scheduler.c`
- Modify: `kernel/fs.c`
- Modify: `userspace/shell.c`

**Interfaces:**
- Consumes: Task 1 fd API, Task 2 OFD behaviour, Task 3 slot API.
- Produces: updated `SYS_OPEN` flags, `SYS_SEEK` whence, `SYS_DUP 13`; fd-aware shell; `fd_init_process()` for PID 0 and new processes; `fd_close_all()` on reap; no public fd-shaped fs functions.

- [ ] **Step 1: Wire syscalls and process lifecycle**

In `kernel/syscall.c`, route fd ≥ 3 through `fd_*` for read/write/close/seek/size/truncate, map flags for `SYS_OPEN`, add `SYS_DUP`, and leave fd 0/1/2 handling inside `fd.c`.

In `kernel/scheduler.c`:
  - call `fd_init_process(&processes[0])` in `scheduler_init()`;
  - call `fd_init_process()` in `process_build_common()` before the process is queued;
  - call `fd_close_all()` in `scheduler_reap()` before clearing the slot.

- [ ] **Step 2: Update shell flags and remove old fs API**

In `userspace/shell.c`, replace every `SYS_OPEN` argument 2 with the flags from the design table. In `kernel/fs.c`, remove the public fd-shaped functions now that no kernel caller uses them; keep private helpers used by `fs_init()` and `fs_selftest()`.

Run:
```cmd
cmd /c tests\run_all.bat
```
Expected: full gate green, including `NO-SSE`, shell commands, `beta` present, `alpha` absent, `exit`, and `BOOT OK`.

- [ ] **Step 3: Commit the integration**

```bash
git add kernel/syscall.c kernel/scheduler.c kernel/fs.c userspace/shell.c
git commit -m "feat: switch kernel and shell to POSIX fd semantics"
```

---

### Task 6: Living docs and final gate

**Files:**
- Modify: `docs/specs/posix-fd-layer-design.md`
- Modify: `tasks/plan.md`
- Modify: `tasks/todo.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: the verified implementation from Tasks 1–5.
- Produces: design status and verification evidence; updated plan/task/readme entries.

- [ ] **Step 1: Update docs**

Set the design doc status to implemented, record host/gate evidence, and update `tasks/plan.md`, `tasks/todo.md`, and the README feature list. Call out that `echo text to file` now overwrites instead of appending.

- [ ] **Step 2: Verify and commit**

```cmd
cmd /c tests\run_all.bat
git diff --check
git status --short
```
Expected: full gate green, no whitespace errors, only intended files modified.

```bash
git add docs/specs/posix-fd-layer-design.md tasks/plan.md tasks/todo.md README.md
git commit -m "docs: record POSIX fd layer design and verification"
```

---

## Final Verification

After all tasks:
```cmd
cmd /c tests\run_all.bat
git status --short
```
Expected: full quality gate green and clean working tree.
