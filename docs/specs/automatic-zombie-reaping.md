# Spec: Automatic Zombie Reaping (Idle Reaper)

**Status:** Implemented and verified 2026-11-20
**Scope:** one thin slice — release exited processes at runtime without adding
`fork`/`exec` or a parent-aware `wait()` ABI.

## Objective

`scheduler_kill()` and `SYS_EXIT` leave a process in `PROC_ZOMBIE` until some
caller invokes `scheduler_reap()`. Today no production code calls it, so the
kernel stack and the owned user code/stack frames are never freed at runtime.
This slice makes process 0's idle loop reap zombies automatically, closing the
leak without changing scheduling policy.

Users: the kernel idle loop (resource hygiene) and host tests (regression
proof). Success: after a user process exits, frame usage returns to baseline
without an explicit `scheduler_reap()` call from production code.

## Tech Stack

- C11 (freestanding kernel), C11 host tests with native GCC.
- Zig 0.16 cross-build for the kernel; `tools/build_kernel.py` orchestrates it.
- QEMU x86_64 with TCP serial for the interactive runtime gate.

## Commands

```cmd
REM Host tests (RED/GREEN loop)
cmd /c tests\run_tests.bat

REM Full kernel build
python tools\build_kernel.py --quiet

REM Full quality gate
cmd /c tests\run_all.bat
```

## Project Structure

- `kernel/kernel.h` — public scheduler API contract.
- `kernel/scheduler.c` — reaper implementation and invariants.
- `kernel/kernel.c` — process 0 idle loop, the only production caller.
- `tests/test_scheduler.c` — host regression tests.
- `tasks/plan.md`, `tasks/todo.md` — active plan and task list.
- `docs/specs/automatic-zombie-reaping.md` — this spec.

## Code Style

Follow the existing scheduler style: explicit `__attribute__((noreturn))` on
non-returning paths, `state_set()` for every state transition, and
`preempt_disable()` / `preempt_enable()` around multi-step state changes.

```c
int scheduler_reap_zombies(void) {
    if (current_process != 0) return -1;

    int reaped = 0;
    preempt_disable();
    for (int pid = 1; pid < MAX_PROCESSES; pid++) {
        if (processes[pid].state == PROC_ZOMBIE &&
            scheduler_reap(pid) == 0) {
            reaped++;
        }
    }
    preempt_enable();
    return reaped;
}
```

API contract: return the number of zombies reaped (0 when none exist), or -1
when called from a non-idle process. The caller never needs to know pids.

## Testing Strategy

- RED first: add a host test that calls `scheduler_reap_zombies()` and verify it
  fails to link before the implementation exists.
- GREEN: implement the minimal API and idle-loop call.
- Host assertions: zombie frames remain allocated until the reaper runs; the
  reaper frees kernel stack + owned runs; ownership slots are cleared; repeated
  calls are harmless; non-idle callers are rejected; scheduler invariants hold.
- Full gate: `cmd /c tests\run_all.bat` must stay green, including the existing
  `exit`/`BOOT OK` runtime path.

## Boundaries

- Always: keep `scheduler_reap(pid)` as the single freeing path; preserve MLFQ
  counters, queue order, and the idle fallback contract; run host tests before
  commits.
- Ask first: adding a `wait()` syscall, parent/child tracking, exit-status
  delivery, or changing when `SYS_EXIT` returns to idle.
- Never: reap `current_process`, reap PID 0, or free frames outside
  `scheduler_reap()`.

## Success Criteria

- `scheduler_reap_zombies()` exists in `kernel.h` and is called from the idle
  loop in `kernel.c` with no other production callers.
- A host test proves automatic reaping returns PMM usage to baseline after an
  exited process owned user frames, without an explicit per-pid reap.
- `cmd /c tests\run_tests.bat` passes with 0 failures.
- `cmd /c tests\run_all.bat` passes: build, `NO-SSE`, and TCP-serial shell
  commands unchanged.
- No new zombie can be reaped twice; `scheduler_check_invariants()` stays 0.

## Verification evidence (2026-11-20)

- RED: the new host test failed to compile before `scheduler_reap_zombies()`
  existed (`implicit declaration`); slab suite stayed at 2269 checks.
- GREEN: scheduler suite 343 checks, 0 failures. The new test kills a process
  with 4 kernel-stack frames plus owned 3-frame code and 32-frame stack runs,
  verifies they stay allocated while it is a zombie, then proves the idle
  reaper returns all 39 frames without an explicit per-pid reap. It also checks
  a second reap finds nothing and a non-idle caller is rejected.
- `cmd /c tests\run_all.bat`: host tests → build (`NO-SSE` OK, 201,976-byte
  kernel ELF, 44,384-byte raw kernel, 66,868-byte `os.elf`) → TCP-serial shell
  session with `help`, `clear`, `touch runtime.txt`, `ls`, `exit`, and
  `BOOT OK: 1528 bytes of serial output`.
- `scheduler_check_invariants()` stayed 0 after the reaper ran; no MLFQ counters
  or queue-ordering behaviour changed.

## Open Questions

- Parent-aware `wait()` and exit-status reporting are intentionally deferred
  until process creation gains a parent/child model.
