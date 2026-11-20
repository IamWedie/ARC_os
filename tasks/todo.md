## Task List

### Phase 1: Remove MSYS2 Dependency ✅

- [x] Task 1: Update build.bat to use native Windows GCC ✅
- [x] Task 2: Update build.py to use native Windows GCC ✅
- [x] Task 3: Update rebuild.bat to use native Windows GCC ✅
- [x] Checkpoint 1: MSYS2 Dependency Removed ✅

### Phase 2: Memory & Scheduler (Year 1) — COMPLETE (UP-only)

#### Memory (host-verified)
- [x] Task 4: Size-classed slab allocator (12 classes, 16 B – 32 KiB) — `kernel/slab.h`, `kernel/slab.c`
- [x] Task 5: kmalloc/kfree route through slab; > 32 KiB use a PMM page run — `kernel/memory.c`
- [x] Task 6: PMM integration; added missing `struct pmm_region` + PMM/VMM declarations
- [x] Task 6b: Host stress test links the REAL `slab.c` + `memory.c` with a PMM stub — 2269 checks pass
- [x] Task 6c: Double-free detection via per-slab allocation bitmap (mutation test proved it is exercised)
- [x] Task 6d: `tests\run_tests.bat` reproduces build + run
- [x] Task 6e: Wire kernel C sources into the real build/link
      `build.bat`/`build.py`/`rebuild.bat` now invoke `tools/build_kernel.py`;
      all `kernel/*.c` + `lib/*.c` compile, link, and embed into `os.elf`.
      Build validates the multiboot header and the embedded kernel bytes.
- [x] Task 6f: Runtime validation in QEMU — `os.elf` reaches `arc> ` in ring 3;
      full serial boot log captured by `tools\boot_test.py`.

#### Scheduler
- [x] Task 7: Process state machine — `PROC_FREE`, `PROC_READY`, `PROC_RUNNING`,
      `PROC_IDLE`, `PROC_BLOCKED`, `PROC_ZOMBIE` in `kernel/kernel.h`;
      transitions enforced by `kernel/scheduler.c`.
- [x] Task 8: O(1) MLFQ run queue — 4 levels, FIFO ring queues, 10-tick
      quantum with demotion, 100-tick priority boost.
- [x] Task 9: Preemption point in `irq0_handler` (every 10 PIT ticks); `irq0_stub` RSP contract verified in `isr.S`
- [x] Task 10: `scheduler_yield()` via software interrupt vector `0x81`;
      `scheduler_get_stats()` now reports real counters; block/wake/kill/reap API.
- [x] Task 10b: Runtime verification of context switches — the timer preempts
      the idle loop into the ring-3 shell, which prints `arc> ` via `int 0x80`.
      The serial-driven runtime test runs `help`, `clear`, `touch runtime.txt`,
      `ls`, and `exit`; `exit` prints `[Process exited]` and returns to idle.

- [x] Checkpoint 2: Memory + Scheduler verified
      Memory: 2269 host checks, 0 failures.
      Scheduler: 251 host checks, 0 failures (states, FIFO fairness, demotion,
      boost, block/wake, kill/reap, idle fallback, invariants).
      Runtime: full quality gate passes — host tests, build, and an interactive
      QEMU shell session.
      Follow-up closed in Phase 3: user code/stack frames are now owned by the
      process and freed on reap (see below).

### Phase 3: POSIX Layer & Filesystem (Year 2)
- [x] Process resource ownership: track and free user code/stack frames on reap
      `process_t.owned[]` holds up to 4 physical frame runs;
      `process_alloc_owned_frames()` attaches atomically; `scheduler_reap()`
      frees them. `kernel/user.c` uses the API and brackets process setup with
      preemption disabled so a timer tick cannot schedule a half-mapped process.
      Host: 318 scheduler checks (up from 251), 0 failures; full gate green.
- [x] Automatic zombie reaping (idle reaper) ✅
  - Acceptance: `scheduler_reap_zombies()` reaps every zombie except the current
    process; process 0's idle loop calls it; an exited process's kernel stack and
    owned user frames return to the PMM without an explicit per-pid reap.
  - Verify: RED test failed before the API existed; GREEN host run 343 checks,
    0 failures; full gate green including `exit` and `BOOT OK`.
  - Files: `kernel/kernel.h`, `kernel/scheduler.c`, `kernel/kernel.c`,
    `tests/test_scheduler.c`, `docs/specs/automatic-zombie-reaping.md`.
- [ ] FAT32 driver with cache integration
- [ ] fork/exec/wait implementation
- [ ] /proc filesystem for process introspection
- [ ] libc-like API (string, stdio) — minimal string/memory + malloc/free exist;
      stdio and the wider POSIX surface are still missing

### Phase 4: GUI & Usability (Year 2-3)
- [ ] VESA framebuffer driver
- [ ] PS/2 mouse driver
- [ ] Windowing system (basic)
- [ ] Shell with piping/redirection

### Phase 5: Networking & Polish (Year 3)
- [ ] NIC driver + TCP/IP light
- [ ] USB host enumeration
- [ ] Full POSIX compliance target
