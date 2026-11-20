# Implementation Plan: Process Resource Ownership — COMPLETE

Spec: `docs/specs/process-resource-ownership.md`

## Tasks

1. **RED test (failing on purpose).** ✅
   Added `test_user_owned_frames()` to `tests/test_scheduler.c`. Before the API
   existed the scheduler test failed to compile with `implicit declaration of
   process_alloc_owned_frames` and `'process_t' has no member named 'owned'`;
   the slab suite stayed green at 2269 checks.

2. **GREEN: scheduler ownership.** ✅
   - `kernel/kernel.h`: `PROCESS_MAX_OWNED_REGIONS`, `proc_region_t`,
     `process_t.owned[]`, `process_alloc_owned_frames()` prototype.
   - `kernel/scheduler.c`: atomic allocate-and-attach API; ownership slots
     cleared on process build; `scheduler_reap()` frees every owned run.
   - Result: 301 checks, 0 failures.

3. **GREEN: real loader uses the API.** ✅
   `kernel/user.c` now creates the process, attaches code and user-stack runs
   through `process_alloc_owned_frames()`, and uses kill+reap on its error path.
   A runtime regression exposed a setup race; the loader now brackets the
   window with `preempt_disable()`/`preempt_enable()` so the process is not
   scheduled before its mappings exist. Added `test_owned_frame_validation()`
   for invalid pids, zero pages, capacity, and FREE-slot rejection.

4. **Verify.** ✅
   - `cmd /c tests\run_tests.bat`: slab 2269, scheduler 318, 0 failures.
   - `cmd /c tests\run_all.bat`: host tests, build, `NO-SSE`, and TCP-serial QEMU
     shell session all pass (`help`, `clear`, `touch runtime.txt`, `ls`, `exit`).
   - Boot test repeated 5 more times after the race fix: 5/5 `BOOT OK`.

5. **Document.** ✅
   Spec status/evidence updated; `tasks/todo.md`, `tasks/plan.md`, and
   `README.md` updated after green. No new ADR: this follows the existing
   process-ownership model; the race fix is documented in the spec and code.

## Not in this plan

Automatic reaping/`wait()`, POSIX fd semantics, FAT32, per-process VAs.
