# Spec: Process Resource Ownership (User Frames)

**Status:** Implemented and verified 2026-11-20
**Scope:** one thin slice — make user code/stack frame ownership explicit and
reclaimable. No new scheduler policy.

## Problem

`kernel/user.c` allocates three kinds of physical frame runs for the ring-3
shell:

| Run | Allocated by | Currently freed? |
|-----|--------------|------------------|
| Kernel stack (4 frames) | either `user.c` or `process_build_common()` | Yes — `stack_base`/`stack_size` |
| User code (1..`USER_FRAME_PAGES`) | `user.c`, `pmm_alloc_pages()` | **No** |
| User stack (`USER_STACK_PAGES`) | `user.c`, `pmm_alloc_pages()` | **No** |

`process_t` has no field for the last two runs, so `scheduler_reap()` cannot
release them. Every spawn/exit cycle permanently loses 3 + 32 frames today.
This blocks `fork`/`exec` and any repeated user-process creation.

VMM page-table frames are deliberately **out of scope**: the current mappings
use fixed VAs, so those tables are allocated once and shared. Per-process
address spaces are a separate later slice.

## Goals

1. A process can own a small, bounded set of additional physical frame runs.
2. Allocation and ownership registration are **atomic**: callers cannot leak
   a run by forgetting to attach it, and a failed attach frees what it took.
3. `scheduler_reap()` releases every owned run and clears the ownership slots.
4. The user loader uses that API and has no manual PMM cleanup on its error
   path.
5. A host test proves repeated create/attach/kill/reap cycles leave the PMM
   frame count exactly where it started.

## Non-goals

- Automatic reaping / `wait()` at the time this slice landed; the idle-loop
  reaper was completed separately in `docs/specs/automatic-zombie-reaping.md`.
  Parent-aware `wait()` is still out of scope.
- Per-process page tables, ASID/CR3 switching, or COW.
- `fork`/`exec`, POSIX fd semantics, FAT32, persistence.

## Interface contract

```c
#define PROCESS_MAX_OWNED_REGIONS 4

typedef struct {
    uint64_t base;   /* page-aligned physical address; 0 = empty slot */
    uint64_t pages;  /* number of 4 KiB frames in the run */
} proc_region_t;

/* In process_t: proc_region_t owned[PROCESS_MAX_OWNED_REGIONS]; */

/*
 * Allocate `pages` contiguous physical frames and register them as owned by
 * `pid`. Returns the page-aligned physical address, or NULL on failure.
 * On failure no frames remain allocated and no slot is consumed.
 * Frames registered here are released exactly once by scheduler_reap().
 * The kernel stack keeps its existing stack_base/stack_size ownership; no
 * owned[] slot is used for it.
 */
void* process_alloc_owned_frames(int pid, size_t pages);
```

Invariants:

- Only live processes (`READY`/`RUNNING`/`BLOCKED`/`ZOMBIE`) may own frames;
  `pid == 0`, `PROC_FREE` slots, invalid pids, `pages == 0`, and a full table
  are rejected with `NULL`.
- `base != 0` for any live slot; reap zeroes `base` and `pages`.
- A frame run appears in at most one slot. No alias/double-free API exists.
- Reap stays callable only for `pid != current_process` (unchanged).

## Acceptance criteria

- `cmd /c tests\run_tests.bat`: slab suite unchanged (2269 checks), scheduler
  suite passes with the new ownership test, 0 failures.
- New test shows net-zero PMM frames after at least 8 create → attach → kill →
  reap cycles, and asserts ownership slots are cleared.
- `cmd /c tests\run_all.bat`: full gate green — build sizes may change slightly,
  `NO-SSE` check passes, TCP-serial runtime test still reaches `arc> ` and
  completes `help`, `clear`, `touch runtime.txt`, `ls`, `exit`.
- No change to scheduler policy, MLFQ counters, or idle behaviour.

## Risks / mitigations

| Risk | Mitigation |
|------|------------|
| Reap frees a run still mapped into a stale PTE | Only one user VA space exists; the next loader call overwrites those PTEs. Add unmapping with per-process VAs later. |
| Attach fails after process creation | Create the process first, attach all runs, then kill+reap on failure; reap frees whatever attached. |
| Capacity too small for future regions | 4 slots cover kernel stack (separate), code, user stack, and one spare. Bump the constant, not the API, when a fourth run appears. |
| Ownership table adds 64 B × 16 processes | 1 KiB total, fixed; cheaper than one leaked 4 KiB run per exit. |

## Discovered hazard and fix

Creating the process before allocating/mapping its frames made it runnable too
early. A timer tick in that window switched to a half-built process: the first
quality-gate run faulted on a user stack write at `0x900001fff8`, and a widened
setup window reproduced it deterministically as an instruction-fetch fault at
`rip = 0x8000000000` with error `0x4`. The loader now wraps the whole setup
window in `preempt_disable()` / `preempt_enable()`; the process is published to
the run queue only after both user mappings are complete. See the comment at
the call site in `kernel/user.c`. This is a loader contract, not a scheduler
policy change.

## Verification evidence (2026-11-20)

- RED: new test failed to compile before the API existed; slab suite stayed at
  2269 checks.
- GREEN: scheduler suite 318 checks, 0 failures, including net-zero PMM pages
  after 8 repeated create/attach/kill/reap cycles and capacity validation.
- `cmd /c tests\run_all.bat`: host tests → build (`NO-SSE` OK,
  201,464-byte kernel ELF, 44,256-byte raw kernel, 66,740-byte `os.elf`) →
  TCP-serial QEMU shell session, all green.
- The boot test was repeated 5 additional times after the race fix: 5/5 `BOOT OK`.
