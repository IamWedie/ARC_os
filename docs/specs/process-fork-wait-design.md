# Design: fork / wait (process lifecycle module 2)

Status: Approved by controller delegation ("do what you gotta do man" +
"don't stop until the OS is working with UI and everything"). Design calls
below are the implementer's; no per-decision approval is required.

## Intent

`address-space` landed per-process page tables. This module makes process
creation observable from userspace: a process can duplicate itself (`fork`),
and a parent can collect a child's exit status (`wait`). This is the last
kernel primitive needed before `exec-loader`, and the first real caller of
`fd_clone_process()`.

Without this, the shell can never spawn a child, so neither `exec` nor any
window/UI process can exist.

## Goals

- `SYS_FORK 14`: clone the calling user process into a new PID.
- `SYS_WAIT 15`: block until a child exits, write its exit code to a user
  pointer, and reap it.
- Parent/child links and parent-aware reaping (idle reaper must not steal a
  zombie before its parent observes it).
- Full host-test coverage plus a QEMU-visible shell command that exercises
  fork + wait end to end.

## Non-goals

- `exec`, `vfork`, `clone`, threads, copy-on-write, SMP.
- No changes to MLFQ policy/counters, existing syscall numbers, fd refcount
  contract, `USER_CODE_VA`/`USER_STACK_VA`, embedded shell image, or
  `tests/run_all.bat`.
- No page-fault handler; access to a not-yet-mapped page still halts.

## Constraints

- UP-only; preemption is disabled across every multi-step process-creation
  window.
- `fork-wait` calls the published `as_*` API; it does not import pagetable
  internals into new files. Deep-copy support is added *inside* the
  address-space module as a new published function `as_clone()`.
- Child address space is a full physical copy (no COW): deep-copy page tables
  (`as_destroy()` would otherwise double-free shared tables) and copy every
  mapped data page into one contiguous owned run.
- The syscall trap frame is transient on the current kernel stack. It is
  captured via a global frame pointer written by `syscall_stub` before the C
  dispatcher runs.

## Approaches considered

1. **Resume-in-syscall frame copy (chosen).** Copy the parent's live syscall
   frame (15 GPRs + iretq frame) into a fresh kernel stack for the child, set
   saved `rax = 0`, and queue it READY. The child resumes at the instruction
   after `int 0x80` with `fork()` returning 0. Copies the user stack too, so
   local variables are independent. Cost: a few hundred bytes of assembly
   contract and one global.
2. **Re-enter child at a trampoline.** Start the child at a new PC that calls
   a callback passed to `fork`. Rejected: requires a per-process callback ABI
   and diverges from POSIX `fork`, which the eventual `exec` shell needs.
3. **Share parent pages read-only until first write.** Rejected: needs a page
   fault handler and COW metadata, both explicitly out of scope.

## Design decisions (implementer's calls)

- **D1 — frame layout.** `syscall_frame_t` mirrors the stack left by
  `pushq $0; PUSHREGS` in `syscall_stub`: 15 GPRs (`r15..rax`), return-value
  slot, then `rip/cs/rflags/user_rsp/ss` (168 bytes). A global
  `volatile uint64_t syscall_frame_rsp` is set by the stub. The child's frame
  is written in the *timer-stub* layout (160 bytes, no return slot) because
  context switches resume through timer/yield stubs. Saved `rax = 0`.
- **D2 — process creation.** New `process_fork(parent_pid, frame)` in
  `scheduler.c` allocates a PID and kernel stack, copies the 15 GPRs + iretq
  frame, sets `ppid`, `rip`, `user_mode`, clears `exit_code`/MLFQ state, calls
  `fd_init_process`, and queues at level 0. Returns -1 for kernel-mode parents
  or resource exhaustion.
- **D3 — address-space clone.** New `as_clone(src_pid, dst_pid)` in
  `address_space.c`, backed by a new pagetable walk helper
  `pt_foreach_user_leaf()` that reports `(vaddr, paddr, flags)` for each 4 KiB
  user leaf. `as_clone` allocates one contiguous run sized by
  `pt_count_user_pages()`, copies each source page into it, and maps it at the
  same VA in the child via `pt_map()`. Registers the run in child `owned[0]`.
- **D4 — fd clone.** `sys_fork` calls `fd_clone_process()` after the child
  slot exists. On any clone failure the child is killed + reaped and `-1`
  returned; the OFD one-slot-ref-per-fd contract is untouched.
- **D5 — wait semantics.** `sys_wait(int *status)` scans for a zombie child;
  if found, copies out `exit_code` via `fd_write`-style user pointer (direct
  store; user VA is mapped), reaps it, and returns the child PID. If no zombie
  child exists but live children do, block the caller and yield, then rescan
  on wake. If no children exist, return -1 (`ECHILD`). `status == NULL` is
  allowed (discard status).
- **D6 — reaping.** On child exit (`scheduler_exit_current`) reparent the
  exiting process's children to PID 0, set exit_code, mark zombie, then wake a
  parent that is `PROC_BLOCKED`. `scheduler_reap_zombies()` only reaps zombies
  whose `ppid == 0`, so a parent's zombie is never stolen. `scheduler_reap`
  clears `ppid` on reap.
- **D7 — ABI placement.** `SYS_FORK 14`, `SYS_WAIT 15` appended to `kernel.h`
  and the userspace shell's private syscall list. No existing number moves.
- **D8 — test seam.** A new `fork_wait` host suite links real
  `scheduler.c`, `fd.c`, `address_space.c`, `pagetable.c`, `syscall.c`,
  `fork.c` and a combined hardware stub (`fork_wait_stub.c`) providing PMM,
  TSS, `__stack_top`, `vmm_*`, console/fs hooks. It sets `syscall_frame_rsp`
  to a synthetic frame and tests: fork returns child PID/0, deep-copy
  independence, fd OFD refcounts, wait blocks/wakes and returns status,
  ECHILD, idle reaper does not steal parented zombies, reparenting on exit.

## What is left to the builder

- Ordered task plan and test-first implementation; every task ends with host
  tests + full build + QEMU boot gate.
- Whole-branch review with zero Critical/Important findings before merge.
- Push only after the green gate, per the earlier "Merge and push" precedent
  for `address-space`; if a remote/integration choice is genuinely new, ask.
