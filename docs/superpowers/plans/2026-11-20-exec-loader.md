# Exec Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `SYS_EXEC 16` so a ring-3 process can replace its own image with a second embedded flat program, with arguments, fd close-on-exec, and failure atomicity.

**Architecture:** Build the replacement page table and physical frames completely offline, then commit with one CR3 reload; rewrite the live `int 0x80` frame so `iretq` enters the new image at `USER_CODE_VA` without a new PID. A shared stack builder supplies argc/argv to both the initial shell and exec'd programs.

**Tech Stack:** x86_64 freestanding C (`zig cc`), GNU assembly, MSYS2 host GCC for tests, QEMU boot gate.

**Spec:** `docs/specs/process-exec-loader-design.md`

## Global Constraints

- Append-only syscalls: `SYS_EXEC 16`. Never renumber existing syscalls 1–15.
- Do not change MLFQ policy/counters, fd one-slot-ref-per-OFD contract, `USER_CODE_VA` (`0x8000000000ULL`), `USER_STACK_VA` (`0x9000000000ULL`), the embedded shell blob, or `tests/run_all.bat`.
- UP-only; no SMP, PCID, COW, page-fault handler.
- Limits: `EXEC_MAX_ARGS 8`, `EXEC_MAX_ARG_LEN 128`, image <= `USER_FRAME_PAGES * 4096` (131072) and <= `FS_MAX_FILESIZE` (65536).
- Batch files run via `cmd /c ...`, never PowerShell `&&`.
- Every task ends with host tests green, full build green, QEMU boot gate green, and one commit.

## Review Focus

- Unmapped or kernel-only user pointer for `path`/`argv` must return -1 without faulting or mutating → Task 3 host test `test_exec_rejects_bad_user_pointer`.
- Missing file, oversized image, or allocation failure must leave root, owned frames, fds and trap frame byte-for-byte unchanged → Task 3 host test `test_exec_failure_is_atomic`.
- Root swap must keep the kernel's slot-0 mappings and reload CR3 before old tables are freed → Task 1 host test `test_as_replace_user_keeps_shared_slot`; QEMU gate covers real CR3.
- Stack layout off-by-one (argc / argv[0] / NULL / strings) would hand the new program garbage → Task 2 host test `test_exec_stack_layout`.
- Close-on-exec must drop only fds >= 3, preserving 0/1/2 and releasing exactly one OFD reference per file fd → Task 3 host test `test_exec_close_on_exec`.

---

### Task 1: Atomic address-space replacement

**Files:**
- Modify: `kernel/address_space.c`, `kernel/kernel.h`
- Test: `tests/test_address_space.c`

**Interfaces:**
- Consumes: `vmm_master_root`, `vmm_switch_address_space`, `pt_clone_shared`, `pt_free_user_tables`, `as_alloc_table`, `as_ops`
- Produces: `int as_replace_user(int pid, uint64_t code_pa, size_t code_pages, uint64_t stack_pa, size_t stack_pages);` — 0 on success, -1 on any failure with no mutation to `processes[pid]`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_address_space.c`: map two known code pages and one stack page in a process; call `as_replace_user(pid, code_pa, 2, stack_pa, 1)`. Assert: return 0; `processes[pid].pml4_phys` changed; `pt_lookup(new_root, USER_CODE_VA)` resolves to `code_pa`, `pt_lookup(new_root, USER_STACK_VA)` resolves to `stack_pa`; the PML4 slot-0 entry of the new root equals that of the old root; the stub's recorded `vmm_switch_address_space` pid equals `pid`. Add a second test with a zero `code_pa` that asserts -1 and that the original root is still installed.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: compile error — `as_replace_user` undeclared.

- [ ] **Step 3: Implement `as_replace_user`**

Build a detached root: `as_alloc_table()`, `pt_clone_shared(master, new_root)`, `pt_map` code/stack with `PT_PG_RW | PT_PG_USER`; on any failure free only the new tables/root and return -1. On success save `old_root`, assign `processes[pid].pml4_phys = new_root`, call `vmm_switch_address_space(pid)`, then `pt_free_user_tables(old_root, &as_ops)` and `as_free_table(old_root)`. Do not touch `owned[]`. Declare the prototype in `kernel/kernel.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`, then `cmd /c tests\run_all.bat`
Expected: all 10 host suites PASS; `QUALITY GATE PASSED`.

- [ ] **Step 5: Commit**

`git add kernel/address_space.c kernel/kernel.h tests/test_address_space.c && git commit -m "feat(exec): add atomic user address-space replacement"`

---

### Task 2: Image loader and argc/argv stack builder

**Files:**
- Create: `kernel/exec.c`, `tests/test_exec.c`, `tests/exec_stub.c`
- Modify: `kernel/kernel.h`, `tests/run_tests.bat`

**Interfaces:**
- Consumes: `as_replace_user`, `pmm_alloc_pages/pmm_free_pages`, `processes[]`
- Produces:
  - `uint64_t exec_build_user_stack(uint8_t* stack_base, size_t stack_bytes, char* const argv[], int argc);` — returns the user VA of `argc`.
  - `int exec_load_image(int pid, const uint8_t* image, size_t size, char* const argv[], int argc, uint64_t* user_rsp_out);` — 0 on success, -1 on failure with no mutation.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_exec.c` with two tests:
(a) `test_exec_stack_layout`: call `exec_build_user_stack(buf, 4096, {"hello.bin", "alpha", NULL}, 2)` on a zeroed 4 KiB buffer; assert the returned VA is inside `[USER_STACK_VA, USER_STACK_TOP)`, that reading `argc == 2`, `argv[0]`/`argv[1]` point into the buffer, strings are NUL-terminated and equal the inputs, and `argv[2] == NULL`.
(b) `test_exec_load_image`: give a user process two owned runs with known bytes, call `exec_load_image(pid, synthetic_image, image_size, argv, 2, &rsp)`; assert 0, new `pml4_phys`, code bytes at `USER_CODE_VA` equal the image, `rsp` within the new stack VA range, and the argc/argv layout readable at `rsp`. Add a failure case: `size > USER_FRAME_PAGES * 4096` returns -1 with unchanged root and owned slots.

Create `tests/exec_stub.c` from the `tests/fork_wait_stub.c` combined-stub pattern: PMM, `set_tss_rsp0`, `__stack_top`, `vmm_master_root`/`vmm_switch_address_space`/`vmm_invalidate_page`, console and fs hooks, plus fake `_binary_user_hello_bin_start/end/size` symbols. Do not redefine globals owned by real `scheduler.c`. Append an 11th suite to `tests/run_tests.bat` linking real `scheduler.c`, `address_space.c`, `pagetable.c`, `exec.c`, `fd.c` with this stub.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat`
Expected: compile/link error — `exec_build_user_stack` / `exec_load_image` undefined.

- [ ] **Step 3: Implement**

In `exec.c`, `exec_build_user_stack` copies strings top-down from `stack_base + stack_bytes`, aligns down to 8 bytes, writes `argv[]` + NULL, then `argc`, and returns `USER_STACK_TOP - (top - final_sp)`. `exec_load_image` validates arguments, allocates code and stack runs with `pmm_alloc_pages`, zeroes them, copies `size` image bytes, builds the stack, saves and clears all `owned[]` slots, calls `as_replace_user(pid, code, code_pages, stack, USER_STACK_PAGES)`, frees the saved old runs on success (or restores them and frees new runs on failure), and installs new runs in `owned[0]`/`owned[1]`. Declare both prototypes in `kernel/kernel.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`, then `cmd /c tests\run_all.bat`
Expected: all 11 host suites PASS; `QUALITY GATE PASSED`.

- [ ] **Step 5: Commit**

`git add kernel/exec.c kernel/kernel.h tests/test_exec.c tests/exec_stub.c tests/run_tests.bat && git commit -m "feat(exec): add image loader and argc/argv stack builder"`

---

### Task 3: `SYS_EXEC` syscall body and dispatch

**Files:**
- Modify: `kernel/exec.c`, `kernel/syscall.c`, `kernel/kernel.h`, `tests/test_exec.c`

**Interfaces:**
- Consumes: `exec_load_image`, `exec_build_user_stack`, `fs_find_slot`, `fs_slot_size`, `fs_slot_read`, `fd_close`, `pt_lookup`, `syscall_frame_rsp`
- Produces: `long sys_exec(const char* path, char* const argv[]);` — 0 on success, -1 on failure.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_exec.c`:
- `test_exec_frame_rewrite`: synthetic `syscall_frame_t` with known rip/rsp, fake FS containing a valid image, a user process with fds 0/1/2 plus two file fds; assert `sys_exec("hello.bin", argv) == 0`, `frame->rip == USER_CODE_VA`, `frame->user_rsp` equals the value returned by the loader, `frame->cs`/`ss` unchanged, `frame->rax == 0`, `frame->ret_slot == 0`, fds 0/1/2 unchanged, file fds >= 3 free with their OFD refs released.
- `test_exec_failure_is_atomic`: missing path, oversized image, and bad argv pointer each return -1 with unchanged frame, `pml4_phys`, and fd table.
- `test_exec_rejects_bad_user_pointer`: path VA unmapped in the caller's root returns -1 without touching anything.
- `test_exec_close_on_exec`: asserts only fds >= 3 close and one slot ref is released per file OFD.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\test_exec.exe` (after the 11th-suite build from Task 2)
Expected: FAIL — `sys_exec` undefined / dispatch case absent.

- [ ] **Step 3: Implement**

`sys_exec` disables preemption first, validates `current_process`/`user_mode`/`syscall_frame_rsp`, copies path and argv with bounded, `pt_lookup`-checked user reads into kernel stack buffers, resolves the file via `fs_find_slot` and `fs_slot_size`, stages bytes into PMM pages with `fs_slot_read`, calls `exec_load_image`, closes fds 3..FD_MAX-1 with `fd_close`, patches the frame fields listed above, re-enables preemption and returns 0. Every failure path frees staging pages, re-enables preemption and returns -1 without mutation. Add `case SYS_EXEC: return sys_exec((const char*)a1, (char* const*)a2);` to `syscall_dispatch` and the prototype/`#define SYS_EXEC 16` to `kernel/kernel.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`, then `cmd /c tests\run_all.bat`
Expected: all 11 host suites PASS; `QUALITY GATE PASSED`.

- [ ] **Step 5: Commit**

`git add kernel/exec.c kernel/syscall.c kernel/kernel.h tests/test_exec.c && git commit -m "feat(exec): wire SYS_EXEC with checked args and close-on-exec"`

---

### Task 4: Build pipeline, boot registry, and user entry ABI

**Files:**
- Modify: `tools/build_kernel.py`, `kernel/exec.c`, `kernel/kernel.c`, `kernel/user.c`, `kernel/kernel.h`, `userspace/crt0.S`, `userspace/shell.c`, `tests/test_exec.c`

**Interfaces:**
- Consumes: `exec_build_user_stack`, fs slot API, `_binary_user_hello_bin_*`
- Produces: `void exec_install_programs(void);` and a working `user_main(int argc, char** argv)` entry for both user programs.

- [ ] **Step 1: Write the failing test**

Add `test_exec_install_programs` to `tests/test_exec.c`: after calling `exec_install_programs()`, assert `fs_find_slot("hello.bin") >= 0`, its size equals the fake blob size, and `fs_slot_read` returns the fake blob bytes. Also assert `user_main` is reachable by checking the shell source symbol exists? No source introspection — instead add a build-level check in `tools/build_kernel.py` that raises `BuildError` if `user_hello.bin` is missing or empty, and show the pre-change build failing that check.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c tests\run_tests.bat` (link error for `exec_install_programs`) and `python tools\build_kernel.py --quiet` (missing hello blob check fails).

- [ ] **Step 3: Implement**

Refactor `build_user_shell` into a helper that builds any `(name, [sources])` pair with the existing flags/linker; produce `user_shell.bin` and `user_hello.bin`, convert both to objects, return a list to `link_kernel` (update its signature and `main`). Change `crt0.S` to set segment registers, `popq %rdi` (argc), `movq %rsp, %rsi` (argv), `andq $-16, %rsp`, `call user_main`, then exit via `SYS_EXIT`. Change `shell.c` to expose `void user_main(int argc, char** argv)` calling `shell_main()` (mark params unused). Change `user_process_create_from_blob` to place `argc = 1`, `argv[0] = "shell"` with `exec_build_user_stack` before mapping the stack. Implement `exec_install_programs` using `fs_open_slot("hello.bin", 1)` → `fs_slot_add_ref` → `fs_slot_write` → `fs_slot_release`, and call it from `kernel_main` after `fs_init()`. Declare the blob symbols and prototype in `kernel/kernel.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmd /c tests\run_tests.bat`, `cmd /c tests\run_all.bat`
Expected: all 11 host suites PASS; base QEMU gate still green with `BOOT OK` and the unchanged shell expectations — proving the new crt0 ABI did not break boot.

- [ ] **Step 5: Commit**

`git add tools/build_kernel.py kernel/exec.c kernel/kernel.c kernel/user.c kernel/kernel.h userspace/crt0.S userspace/shell.c tests/test_exec.c && git commit -m "feat(exec): build second blob and switch user entry to argc/argv"`

---

### Task 5: Hello program, shell command, and QEMU proof

**Files:**
- Create: `userspace/hello.c`, `tests/boot_test_exec.bat`
- Modify: `userspace/shell.c`, `tools/build_kernel.py`

**Interfaces:**
- Consumes: `SYS_EXEC 16`, `SYS_FORK 14`, `SYS_WAIT 15`, `user_main` ABI
- Produces: shell command `execdemo`; program output `exec: hello ok`; gate `tests/boot_test_exec.bat`.

- [ ] **Step 1: Write the failing check**

Create `tests/boot_test_exec.bat` mirroring `tests/boot_test_fork.bat` base expects/inputs, plus `--expect "exec: hello ok" --expect "exec: wait ok"` and `--input "execdemo"`. Run it before implementing.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmd /c tests\boot_test_exec.bat`
Expected: FAIL — missing expected strings.

- [ ] **Step 3: Implement**

`userspace/hello.c`: `void user_main(int argc, char** argv)` prints `exec: hello ok` only when `argc == 2` and `argv[1]` equals `"alpha"`, otherwise `exec: hello bad`, then calls `SYS_EXIT 0`. Add `hello.c` to the second blob's source list in `tools/build_kernel.py`. Add shell command `execdemo`: print `exec: parent`, fork; child builds `{"hello.bin", "alpha", NULL}`, calls `sys_exec`, prints `exec: failed` and exits 9 if it returns; parent waits and prints `exec: wait ok` only for status 0, else `exec: wait bad`. Add it to `help`.

- [ ] **Step 4: Run both gates**

Run: `cmd /c tests\boot_test_exec.bat`, then `cmd /c tests\run_all.bat`, then `cmd /c tests\boot_test_fork.bat`
Expected: all three green; serial proof `exec: parent` → `exec: hello ok` → `[Process exited]` → `exec: wait ok`.

- [ ] **Step 5: Commit**

`git add userspace/hello.c userspace/shell.c tools/build_kernel.py tests/boot_test_exec.bat && git commit -m "feat(exec): add execdemo proof and QEMU gate"`

---

### Task 6: Whole-branch review

**Files:** none

**Interfaces:** consumes the whole exec-loader diff against merge base `c2afc1c`.

- [ ] **Step 1: Dispatch a fresh reviewer subagent** with the spec, plan, ledger and full diff; require a verdict with counts of Critical / Important / Minor findings and triage of any parked minors.
- [ ] **Step 2: Fix Critical/Important findings in one fix wave**, re-run host tests, full build and all three QEMU gates, then re-review the fix diff until zero remain.
- [ ] **Step 3: Verify final gates on the branch tip myself** (`cmd /c tests\run_tests.bat`, `cmd /c tests\run_all.bat`, `cmd /c tests\boot_test_exec.bat`, `cmd /c tests\boot_test_fork.bat`) and record results in the ledger before reporting integration status.
