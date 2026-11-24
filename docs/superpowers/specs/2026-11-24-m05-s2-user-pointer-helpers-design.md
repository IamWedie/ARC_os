# M0.5 §2 first slice — user-memory helpers for SYS_READ/SYS_WRITE

## Intent

Ring-3 syscalls currently pass user pointers directly to CPL0 code, allowing
kernel-address reads/writes and CPL0 faults. This slice introduces a reusable,
host-testable user-memory boundary and routes only `SYS_READ` and `SYS_WRITE`
through it. No other syscall, syscall number, ABI definition, filesystem
behavior, or user-visible normal-build behavior changes in this slice.

`tasks/M0.5-kernel-safety.md` does not exist in this checkout and is not created
or assumed. `docs/plan.md` remains untracked in the original checkout and is not
staged, modified, or copied into this worktree.

## Scope and constraints

- New module: `kernel/user_access.c`; declarations in `kernel/kernel.h`.
- Root comes from `processes[current_process].pml4_phys`; no invented address
  limit. Validate from the existing paging model:
  - x86-64 low-half canonical form: `(va >> 47) == 0`;
  - user PML4 slots: `(page_va >> 39) >= PT_USER_SLOT_FIRST`;
  - overflow: `len > UINT64_MAX - va`;
  - every page-table level present, user, and not a large-page entry;
  - `PT_PG_RW` at every level for `copy_to_user`.
- Validate before copying so an invalid later page cannot cause a partial
  kernel read or partial user write.
- `SYS_WRITE`/`SYS_READ` use a fixed 512-byte kernel bounce buffer; no buffer is
  allocated from an untrusted length.
- Existing syscall return conventions are preserved: invalid pointers return
  `-1`; zero-length transfers return `0`; EOF still returns a short count; a
  failed copy-out is never reported as success.
- `ARC_SYSCALL_TEST` is compile-time only and adds a hidden shell command for
  the QEMU gate. Normal builds remain byte-for-byte unaffected by that flag.

## Approaches

- **A (chosen): independent `kernel/user_access.c`.** Keeps the boundary API
  separate from `exec.c`, links cleanly into host tests, and is easy to revert.
- **B (rejected): place helpers in `kernel/exec.c`.** Couples a generic boundary
  to the loader and drags exec’s heavy stub world into host tests.
- **C (rejected): host tests only.** Fails the required user-mode QEMU proof.

## API contract

```c
int copy_from_user(void* dst, const void* user_src, size_t len);
int copy_to_user(void* user_dst, const void* src, size_t len);
int copy_user_string(const void* user_src, char* dst, size_t cap);
```

`copy_from_user` and `copy_to_user` return `0` on success and `-1` on invalid
input. `copy_user_string` returns the string length excluding NUL on success and
`-1` if no NUL appears within `cap` or a byte cannot be validated. Zero length
succeeds as a no-op even with a null user pointer; a null kernel buffer with
nonzero length fails.

## Implementation shape

1. Add `kernel/user_access.c` with a non-allocating page-table walk that
   validates PML4, PDPT, PD, and PT entries before any byte access.
2. Use two passes: validate the whole range, then copy. Use the existing
   identity-map assumption for validated physical addresses, as `exec.c` does.
3. In `kernel/syscall.c`, `SYS_WRITE` copies each chunk with
   `copy_from_user()` into a stack bounce buffer before `fd_write()`;
   `SYS_READ` reads into the bounce buffer with `fd_read()` and then copies the
   result out with `copy_to_user()`. Keep the current console-in `sti; hlt`
   retry behavior.
4. Add `tests/test_user_access.c` to `tests/run_tests.bat`, linking the real
   `kernel/user_access.c`, real `kernel/pagetable.c`, and `tests/pt_stub.c`.
   Stubs provide only process globals and page-table memory.
5. Add `tests/boot_test_syscall_ptr.py`/`.bat` and an `ARC_SYSCALL_TEST` define
   in `tools/build_kernel.py` for the shell-only test command. The gate rebuilds
   the test kernel, expects invalid-pointer writes/reads to return `-1` while
   the shell remains usable, then rebuilds the normal kernel.

## Tests

Host cases: valid copy; unmapped pointer; PML4 slot 0 kernel pointer;
cross-page pointer into an unmapped page; read-only copy-out destination;
zero-length/null; address-plus-length overflow; unterminated string at cap.
QEMU case: ring-3 shell calls `SYS_WRITE` and `SYS_READ` with a kernel virtual
address and observes failure without kernel corruption.

Verification commands will include the build, `tests\run_tests.bat`,
`tests\run_all.bat`, all existing GUI/fork/exec/MB2/persistence/AHCI gates, and
the new syscall-pointer gate. Exact commands, exit codes, and output tails will
be reported.

## Non-goals

No pathname syscalls, wait-status changes, exec changes, page-table API changes,
PMM changes, disk writes, ABI changes, commits, pushes, or merge/tag operations.
