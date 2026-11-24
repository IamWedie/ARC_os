# M0.5 §2 First Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable user-memory boundary and route only `SYS_READ`/`SYS_WRITE` through it.

**Architecture:** A new `kernel/user_access.c` validates the current process’s page-table walk before copying bytes. `kernel/syscall.c` uses a fixed kernel bounce buffer so no untrusted length drives allocation. A compile-time-only shell command enables a ring-3 QEMU gate without changing normal builds.

**Tech Stack:** Freestanding C, Zig/lld kernel build, MSYS2 GCC host tests, Python + QEMU gates.

**Spec:** `docs/superpowers/specs/2026-11-24-m05-s2-user-pointer-helpers-design.md`

## Global Constraints

- No edits to `exec.c`, pathname syscalls, `SYS_WAIT`, syscall numbers, ABI, page-table API, PMM, filesystem, or disk images.
- Do not create or assume `tasks/M0.5-kernel-safety.md`; it is absent.
- Do not stage, modify, move, or commit `docs/plan.md`; it stays untracked in the original checkout.
- Do not weaken, skip, delete, or change the meaning of any existing test or gate.
- No commit, push, merge, tag, or publish without separate approval.
- Normal kernel/user builds must be unaffected unless `ARC_SYSCALL_TEST=1`.
- Only disposable QEMU images may be written.
- Record exact commands, exit codes, and relevant output.

## Review Focus

- Kernel virtual address passed as a user buffer — Task 1 kernel-VA test and Task 3 QEMU gate must return `-1` and leave the kernel intact.
- Cross-page buffer whose second page is unmapped — Task 1 cross-page test must reject before any partial read/write.
- Read-only user output page — Task 1 `copy_to_user` test must reject and leave bytes unchanged.
- Address-plus-length overflow near `UINT64_MAX` — Task 1 overflow test must reject before walking pages.
- Unterminated maximum-length string — Task 1 `copy_user_string` test must fail without overflowing the destination.

---

## File Structure

- Create `kernel/user_access.c`: validation walk and three copy helpers.
- Modify `kernel/kernel.h`: declarations for the three helpers.
- Create `tests/test_user_access.c`: adversarial host tests using real helper and page-table code.
- Modify `tests/run_tests.bat`: build/run the new host suite without changing existing suites.
- Modify `userspace/shell.c`: `ARC_SYSCALL_TEST`-only `ptrtest` command.
- Modify `tools/build_kernel.py`: `ARC_SYSCALL_TEST=1` compiles the shell with the test define.
- Create `tests/boot_test_syscall_ptr.py` and `tests/boot_test_syscall_ptr.bat`: ring-3 QEMU gate.
- Modify `kernel/syscall.c`: `SYS_READ`/`SYS_WRITE` bounce-buffer routing.

### Task 1: User-access helper module and adversarial host tests

**Files:**
- Create: `kernel/user_access.c`
- Create: `tests/test_user_access.c`
- Modify: `kernel/kernel.h`
- Modify: `tests/run_tests.bat`

**Interfaces:**
- Produces:
  - `int copy_from_user(void* dst, const void* user_src, size_t len);`
  - `int copy_to_user(void* user_dst, const void* src, size_t len);`
  - `int copy_user_string(const void* user_src, char* dst, size_t cap);`

- [ ] **Step 1: Write the failing host test**

Create `tests/test_user_access.c` using `pt_stub.c` for real page tables and local `processes[]`/`current_process` globals only. Use these exact test VAs:

```c
#define KERNEL_TEST_VA       0x0000000000200000ULL /* PML4 slot 0 */
#define USER_GOOD_VA         0x0000008000000000ULL /* PML4 slot 1, RW user */
#define CROSS_PAGE_VA        0x0000008000000FFCULL /* good page + unmapped next page */
#define USER_UNMAPPED_VA     0x0000008000001000ULL /* no leaf mapping */
#define USER_RO_VA           0x0000009000000000ULL /* present/user, not RW */
#define USER_UNTERMINATED_VA 0x0000009000001000ULL /* filled with non-NUL bytes */
```

Map only `USER_GOOD_VA`, `USER_RO_VA`, and `USER_UNTERMINATED_VA` using the real `pt_map_page()` API; leave `USER_UNMAPPED_VA` and the page after `CROSS_PAGE_VA` unmapped. Include these assertions:

```c
CHECK(copy_from_user(kernel_buf, (const void*)USER_GOOD_VA, 16) == 0, "valid copy");
CHECK(memcmp(kernel_buf, expected, 16) == 0, "valid bytes");
CHECK(copy_from_user(kernel_buf, (const void*)USER_UNMAPPED_VA, 4) == -1, "unmapped");
CHECK(copy_from_user(kernel_buf, (const void*)KERNEL_TEST_VA, 4) == -1, "kernel VA");
CHECK(copy_from_user(kernel_buf, (const void*)CROSS_PAGE_VA, 8) == -1, "cross-page");
CHECK(copy_to_user((void*)USER_RO_VA, "x", 1) == -1, "read-only dest");
CHECK(copy_from_user(0, 0, 0) == 0 && copy_to_user(0, 0, 0) == 0, "zero length");
CHECK(copy_from_user(kernel_buf, (const void*)USER_GOOD_VA, SIZE_MAX) == -1, "overflow");
CHECK(copy_user_string((const void*)USER_UNTERMINATED_VA, buf, 8) == -1, "unterminated");
```

Also assert no partial destination change after each rejected copy.

- [ ] **Step 2: Run the focused test to verify it fails**

Run from the worktree root:

```cmd
"C:\msys64\ucrt64\bin\gcc.exe" -O2 -Wall -Wextra -std=gnu11 -Ikernel tests\test_user_access.c tests\pt_stub.c kernel\user_access.c kernel\pagetable.c -o tests\test_user_access.exe && tests\test_user_access.exe
```

Expected: link failure because `kernel/user_access.c` does not exist yet and helper symbols are undefined.

- [ ] **Step 3: Implement the helpers**

Create `kernel/user_access.c` with a non-allocating walk from `processes[current_process].pml4_phys`. Validate per page: canonical low half `(va >> 47) == 0`, slot `(page_va >> 39) >= PT_USER_SLOT_FIRST`, no `len > UINT64_MAX - va`, every intermediate and leaf entry `PRESENT|USER`, no PSE entry, and `RW` on every level for `copy_to_user`. Validate the whole range before copying. Use a local address mask matching `exec.c`; do not call or edit `exec.c`. Add the three declarations to `kernel/kernel.h`.

- [ ] **Step 4: Run the focused test to verify it passes**

Run the Step 2 command.

Expected: `tests\test_user_access.exe` exits `0`; all assertions pass.

- [ ] **Step 5: Add the suite to the normal runner**

Add a build/run block to `tests\run_tests.bat` that compiles the same sources as Step 2 and runs `tests\test_user_access.exe`. Do not alter any existing suite block.

- [ ] **Step 6: Run the full host suite**

Run:

```cmd
cmd /c 'tests\run_tests.bat'
```

Expected: exit `0`; all 16 suites pass, including the new user-access suite.

- [ ] **Step 7: No commit**

Do not commit. Leave the diff for review.

### Task 2: Compile-time-only ring-3 QEMU hook and gate

**Files:**
- Modify: `userspace/shell.c`
- Modify: `tools/build_kernel.py`
- Create: `tests/boot_test_syscall_ptr.py`
- Create: `tests/boot_test_syscall_ptr.bat`

**Interfaces:**
- Consumes: `syscall4(long n, long a1, long a2, long a3)` in `userspace/shell.c`.
- Produces: `ARC_SYSCALL_TEST=1` shell command `ptrtest` and gate `tests\boot_test_syscall_ptr.bat`.

- [ ] **Step 1: Write the failing QEMU gate**

Create `tests\boot_test_syscall_ptr.py` following `tests\boot_test_gui.py` conventions. Expect serial output containing `SYSCALL PTR TEST`, `write -1`, `read -1`, `shell alive`, and no panic/exception marker; use the existing QEMU timeout so a blocked raw `SYS_READ` fails rather than hanging. Create `tests\boot_test_syscall_ptr.bat` that always restores the normal build, including when the test build or gate fails:

```bat
@echo off
setlocal
set "GATE_RC=1"
set "ARC_SYSCALL_TEST=1"
python tools\build_kernel.py --quiet
if errorlevel 1 goto restore
python tests\boot_test_syscall_ptr.py
set "GATE_RC=%ERRORLEVEL%"
:restore
set "ARC_SYSCALL_TEST="
python tools\build_kernel.py --quiet
set "RESTORE_RC=%ERRORLEVEL%"
if not "%RESTORE_RC%"=="0" exit /b %RESTORE_RC%
exit /b %GATE_RC%
```

- [ ] **Step 2: Run the gate to verify it fails**

Run:

```cmd
cmd /c 'tests\boot_test_syscall_ptr.bat'
```

Expected: exit `1`; required output is absent or the kernel halts on the invalid pointer.

- [ ] **Step 3: Add the shell hook and build flag**

In `userspace/shell.c`, wrap a `ptrtest` command in `#ifdef ARC_SYSCALL_TEST`. It must call `SYS_WRITE` and `SYS_READ` with a kernel virtual address such as `0x200000UL`, print the three expected lines through the valid `print_str()` path, and remain in the command loop. In `tools/build_kernel.py`, append `-DARC_SYSCALL_TEST` to the shell compile command only when `os.environ.get("ARC_SYSCALL_TEST") == "1"`.

- [ ] **Step 4: Run the gate to verify it is still RED after the hook is added**

Run the Step 2 command.

Expected: exit `1`; `SYSCALL PTR TEST` appears, but `write -1` and `read -1` are absent because raw pointers are still passed to `fd_write`/`fd_read`. This confirms the hook runs before the security fix exists.

- [ ] **Step 5: No commit**

Do not commit. Leave the diff for review.

### Task 3: Route SYS_READ and SYS_WRITE through the helpers

**Files:**
- Modify: `kernel/syscall.c`

**Interfaces:**
- Consumes: `copy_from_user`, `copy_to_user` from Task 1.
- Produces: unchanged `SYS_READ`/`SYS_WRITE` ABI and return conventions.

- [ ] **Step 1: Confirm the gate is still red before routing**

Run:

```cmd
cmd /c 'tests\boot_test_syscall_ptr.bat'
```

Expected: exit `1` because raw pointers are still passed to `fd_read`/`fd_write`.

- [ ] **Step 2: Implement bounded routing**

In `kernel/syscall.c`, define a 512-byte stack bounce buffer. For `SYS_WRITE`, loop: `copy_from_user()` a chunk, `fd_write()` that chunk, accumulate the return, stop on `-1` and return `-1`. For `SYS_READ`, loop: `fd_read()` into the bounce buffer, retain the console-in `sti; hlt` retry, then `copy_to_user()` the returned byte count; return `-1` if copy-out fails, otherwise the accumulated count or short EOF count. Zero-length calls return `0` without dereferencing the user pointer.

- [ ] **Step 3: Run the QEMU gate to verify it passes**

Run the Step 1 command.

Expected: exit `0` with all four expected lines and no panic/exception marker.

- [ ] **Step 4: Run all required gates**

Run each command below and record exit code and final relevant output:

```cmd
cmd /c 'python tools\build_kernel.py --quiet'
cmd /c 'tests\run_tests.bat'
cmd /c 'tests\run_all.bat'
cmd /c 'tests\boot_test_gui.bat'
cmd /c 'tests\boot_test_fork.bat'
cmd /c 'tests\boot_test_exec.bat'
cmd /c 'tests\boot_test_mb2.bat'
cmd /c 'tests\boot_test_persist.bat'
cmd /c 'tests\boot_test_persist_ahci.bat'
cmd /c 'tests\boot_test_ahci.bat'
cmd /c 'tests\boot_test_ahci_mixed.bat'
cmd /c 'tests\boot_test_ahci_readonly.bat'
cmd /c 'tests\boot_test_syscall_ptr.bat'
```

Expected: every command exits `0`; no existing gate weakened or skipped; MB2 gate still reports Multiboot1 if unchanged.

- [ ] **Step 5: No commit**

Do not commit. Produce the diff and evidence report, then stop for review.
