# Design: exec-loader (process lifecycle module 3)

Status: Approved by controller delegation ("do what you gotta do man" +
"don't stop until the OS is working with UI and everything"). Design calls
below are the implementer's; no per-decision approval is required.

## Intent

`address-space` gave every process a private page table; `fork-wait` gave it a
parent/child lifecycle. This module makes the third POSIX primitive usable:
`SYS_EXEC 16` replaces the CURRENT process image with a new program without
creating a PID. That is the last kernel capability needed before a real UI
process can be launched.

Success looks like: a shell command forks, the child execs a second embedded
program with arguments, that program prints proof it received argv and exits
0, and the parent's `wait` observes status 0 — all verified by host tests and
a QEMU gate.

## Goals

- Append `SYS_EXEC 16` only; never renumber existing syscalls 1–15.
- Load a raw flat binary at `USER_CODE_VA`; entry `USER_CODE_VA`, stack
  `USER_STACK_VA` with `USER_STACK_PAGES`.
- Build `argc`/`argv` on the new user stack; change the user entry ABI to
  `user_main(int argc, char** argv)` for both the shell and the new program.
- Rewrite the live `int 0x80` syscall frame so return-from-syscall enters the
  new image; same PID, same kernel stack, no context switch.
- Close fds >= 3 on successful exec; preserve 0/1/2 and the one-slot-ref-per-
  OFD refcount contract.
- Failure atomicity: on any error return -1 with image, address space, fds and
  trap frame unchanged.
- Add a second embedded user program and a standalone QEMU proof gate.

## Non-goals

- ELF parsing, dynamic linking, relocation, shebangs, environment variables.
- New PID, threads, SMP, PCID, COW, page-fault handler.
- Changes to MLFQ policy/counters, fd refcount contract,
  `USER_CODE_VA`/`USER_STACK_VA`, the existing embedded shell blob, or
  `tests/run_all.bat`.

## Constraints

- UP-only; preemption is disabled across the entire sys_exec mutation window.
- `USER_FRAME_PAGES` (32) bounds image size; the RAM filesystem's
  `FS_MAX_FILESIZE` (65536) is the practical ceiling.
- User pointers are untrusted. There is no page-fault handler, so every user
  read must first be validated with `pt_lookup()` against the caller's root.
- The syscall frame is the 168-byte layout published by `syscall_stub`:
  `syscall_frame_t` in `kernel/kernel.h`. `syscall_stub` writes the dispatcher
  return value into `ret_slot` after `POPREGS`, so a successful exec must
  return 0 from `sys_exec` for the new program to observe `rax == 0`.
- The kernel's slot-0 mappings must survive the root swap: a fresh root is
  cloned from the master map, CR3 is reloaded through
  `vmm_switch_address_space(pid)`, and only then are the old tables freed.

## Approaches considered

1. **Flat raw binary (chosen).** The image is copied byte-for-byte from an FS
   slot to pages at `USER_CODE_VA`; the linker script already emits a flat
   image at that VA. An ELF parser would add relocation/segment complexity with
   no benefit for a fixed-address, single-program OS.
2. **Rewrite the live frame (chosen).** Patch `rip`, `user_rsp`, and the return
   slot of the captured `syscall_frame_t`; `iretq` then enters the new image.
   Creating a new PID and context-switching would break POSIX exec semantics,
   duplicate the parent link, and need a second wait path.
3. **Unmap then remap in place.** Rejected: a failure after unmapping leaves a
   half-dead process and violates failure atomicity. The new root and frames
   are built completely offline, then committed in one swap.

## Design decisions (implementer's calls)

- **D1 — address-space replacement.** New published API
  `int as_replace_user(int pid, uint64_t code_pa, size_t code_pages,
  uint64_t stack_pa, size_t stack_pages);` in `address_space.c`. It builds a
  private root from the master map, maps code and stack with
  `PT_PG_RW | PT_PG_USER`, swaps `processes[pid].pml4_phys`, reloads CR3, then
  frees the old user tables and root. Any failure frees only new allocations
  and leaves the process root untouched. It never touches `owned[]`.
- **D2 — image loader.** New `kernel/exec.c` owns
  `int exec_load_image(int pid, const uint8_t* image, size_t size,
  char* const argv[], int argc, uint64_t* user_rsp_out);`. It allocates code
  and stack frames directly with `pmm_alloc_pages`, copies the image, builds
  the argument stack, calls `as_replace_user`, then frees old owned runs and
  registers the new ones in `owned[0]` (code) and `owned[1]` (stack). On any
  failure it restores the saved `owned[]` entries and frees only the new runs.
- **D3 — argument stack.** Shared helper
  `uint64_t exec_build_user_stack(uint8_t* stack_base, size_t stack_bytes,
  char* const argv[], int argc);` writes strings top-down, then `argv[]` plus
  NULL, then `argc`, and returns the user VA of `argc`. `_start` pops `argc`
  into `rdi`, sets `rsi = rsp` (argv), aligns `rsp` to 16, and calls
  `user_main`. Limits: `EXEC_MAX_ARGS 8`, `EXEC_MAX_ARG_LEN 128`.
- **D4 — syscall body.** `long sys_exec(const char* path, char* const argv[]);`
  disables preemption first, validates the caller and the live frame, copies
  path/argv through checked user reads, resolves `fs_find_slot(path)`, stages
  the file with `fs_slot_read`, calls `exec_load_image`, closes fds 3..FD_MAX-1
  with `fd_close`, patches `frame->rip/cs/user_rsp/ss/rax/ret_slot`, returns 0.
  Every failure path returns -1 with no mutation.
- **D5 — checked user reads.** `pt_lookup()` must report a present, user-
  accessible leaf for each byte's page before the copy. Unmapped or kernel-only
  pointers are rejected as -1 instead of faulting the kernel.
- **D6 — boot program registry.** `void exec_install_programs(void);` writes
  the embedded `hello.bin` blob into the RAM FS at boot via
  `fs_open_slot/create` → `fs_slot_add_ref` → `fs_slot_write` →
  `fs_slot_release`, following the existing `welcome.txt` pattern. Called from
  `kernel_main` immediately after `fs_init()`.
- **D7 — build pipeline.** Refactor `tools/build_kernel.py` so the same
  freestanding flags/linker build `user_shell.bin` and `user_hello.bin`, with
  objcopy symbols `_binary_user_shell_bin_*` and `_binary_user_hello_bin_*`
  linked into the kernel. Both blobs keep the same `.userblob` section name;
  the existing linker script already aggregates `*(.userblob)`.
- **D8 — initial shell ABI.** `user_process_create_from_blob` uses
  `exec_build_user_stack` to place `argc = 1`, `argv[0] = "shell"` on the
  shell's stack. `shell.c` gains `user_main(argc, argv)` wrapping the existing
  `shell_main`, so the boot path still works after the crt0 change.
- **D9 — proof.** Shell command `execdemo` forks; the child execs
  `"hello.bin"` with argv `{"hello.bin", "alpha", NULL}`. `hello.c` checks
  `argc == 2` and `strcmp(argv[1], "alpha") == 0`, prints `exec: hello ok`,
  exits 0; the parent prints `exec: wait ok` only on status 0. A separate
  `tests/boot_test_exec.bat` mirrors the base gate plus these expectations.

## What is left to the builder

- Ordered TDD task plan; each task ends with host tests, full build and QEMU
  boot gate green, and is committed.
- Fresh whole-branch review with zero Critical/Important findings.
- Integration/push remains a separate explicit decision.
