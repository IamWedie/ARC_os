# Implementation Plan: ARC OS - Quality OS Development (2-Year Plan)

## 🏛️ Architecture & Quality Foundations

### Design Principles (2-Year Project)
| Principle | Description |
|-----------|-------------|
| **Correctness first** | Every feature must have verified code paths, not just "it works in QEMU" |
| **Cache-aware design** | All data structures chosen for L1/L2/L3 locality; O() analysis documented |
| **Safety by default** | Assertions on all public interfaces, sanitizer clean, no undefined behavior |
| **Modularity** | Clear boundaries: PMM ⇄ Slab ⇄ Scheduler ⇄ FS ⇄ GUI — each replaceable |
| **Observability** | Every subsystem logs metrics (allocs, frees, switches, latency) at runtime |

### Memory Layout (Final)
| Region | Address Range | Purpose |
|--------|--------------|---------|
| Low memory | 0x000000–0x0009FF | BIOS data, IVT |
| Kernel space | 0x100000–0xFFFFFFFF | Identity-mapped first 512 MB, then higher-half |
| User space | 0x8000000000–0x9FFFFFFFFFFF | Per-process virtual memory (32 TiB) |
| Kernel structures | Various | PMM bitmap, slab caches, per-CPU data |

### Subsystem Interactions
```
PMM (physical frames) 
    ⇄ Slab allocator (object caches, per-CPU freelists) 
    ⇄ kmalloc/kfree (kernel API, size-classed → slab, fallback → PMM) 
    ⇄ Scheduler (process structures via kmem_cache) 
    ⇄ Filesystem (inodes, metadata via slab) 
    ⇄ GUI (window objects, buffers via slab)
```

---

## 📅 Year-by-Year Milestones

### **Year 1: Foundations (Months 1–12)**
| Milestone | Target | Quality Metric |
|-----------|--------|----------------|
| **M1** (Month 3) | Bare metal boots; memory mgmt; 1 process | `mem_selftest()` passes; no PMM leaks after shell exit |
| **M2** (Month 6) | Multitasking (4 processes); FAT32 shell | Scheduler latency < 10 ms; FAT32 read/write ≥ 95% sectors |
| **M3** (Month 9) | VESA GUI; mouse; basic shell | GUI responds < 50 ms to input; zero kernel panics |
| **M4** (Month 12) | POSIX API (fork/exec/wait); shell with piping | 80% of coreutils pass; memory fragmentation < 10% after 24h |

**Checkpoint 1 (End of Year 1):** 
- [ ] OS boots and runs a shell with built-in commands
- [ ] 4+ processes run preemptively with proper context switches
- [ ] No memory leaks detected by custom stress test
- [ ] FAT32 filesystem works for read/write/ls/rm operations

---

### **Year 2: Refinement & Usability (Months 13–24)**
| Milestone | Target | Quality Metric |
|-----------|--------|----------------|
| **M5** (Month 15) | Journaling filesystem (or improved FAT32) | Journal commit atomicity; crash consistency tests |
| **M6** (Month 18) | Windowing system; piped shell; mouse support | Window switch < 50 ms; no resource leaks in GUI loop |
| **M7** (Month 20) | USB enumeration (HID + mass storage) | Descriptor validation; 95% of tested devices enumerated |
| **M8** (Month 24) | "Functional usage" OS — installable, usable | POSIX compliance: 80% of busybox commands; average session 30+ min |

**Checkpoint 2 (End of Year 2):** 
- [ ] POSIX programs compile and run (busybox subset)
- [ ] GUI is responsive with window management
- [ ] USB devices (flash drives, mice) work
- [ ] Stable session: 1 hour continuous use without reboot

---

### **Year 3: Polish & Distribution (Months 25–36)** — *Optional extension*
| Milestone | Target |
|-----------|--------|
| **M9** (Month 30) | Networking (NIC driver + TCP/IP light) |
| **M10** (Month 34) | Package manager, device auto-detection |
| **M11** (Month 36) | Installer, documented API, open-source release |

---

## 🧱 Current Implementation Status (as of 2026-11-20)

### ✅ Completed and Verified

| Component | Status | Evidence |
|-----------|--------|----------|
| **MSYS2 dependency removed** | ✅ Build scripts no longer need an MSYS2 shell; they use Zig + native GNU objcopy | `build.bat`, `build.py`, `rebuild.bat` all run |
| **Slab allocator (size classes)** | ✅ 12 classes (16 B – 32 KiB), per-CPU active slab, global partial list | `kernel/slab.h`, `kernel/slab.c` |
| **kmalloc/kfree on slab** | ✅ ≤ 32 KiB from slab; larger requests use a PMM page run with a header | `kernel/memory.c` |
| **PMM/VMM compile fixes** | ✅ Missing `struct pmm_region`, PMM/VMM declarations, `PG_WRITABLE` typo | freestanding `-fsyntax-only` is clean for slab/memory/kernel/pmm/vmm |
| **Host slab/memory stress test (real code)** | ✅ Links real `slab.c` + `memory.c` against a PMM stub | 2269 checks, 0 failures |
| **Double-free detection** | ✅ Per-slab allocation bitmap rejects invalid frees | Mutation test failed as expected when detection was removed |
| **Preemptive MLFQ scheduler** | ✅ 4 levels, O(1) FIFO ring queues, 10-tick quantum, 100-tick boost | `kernel/scheduler.c`; 251 host checks, 0 failures |
| **Process lifecycle** | ✅ `FREE/READY/RUNNING/IDLE/BLOCKED/ZOMBIE`; block/wake/kill/reap; stats; invariant checker | `tests/test_scheduler.c` |
| **Process resource ownership** | ✅ `process_t.owned[]` + atomic `process_alloc_owned_frames()`; `scheduler_reap()` frees user code/stack runs; loader setup is non-preemptible | 318 scheduler checks, 0 failures; full gate green; 5/5 repeated boots |
| **Automatic zombie reaping** | ✅ `scheduler_reap_zombies()` called from PID 0's idle loop; no explicit per-pid reap needed at runtime | 343 scheduler checks, 0 failures; full gate green |
| **Software yield** | ✅ `scheduler_yield()` raises vector `0x81`; same stack-swap contract as timer stubs | `kernel/isr.S`, `kernel/scheduler.c` |
| **Idle cannot starve work** | ✅ PID 0 is `PROC_IDLE`, never queued; it runs only when no real process is ready | host test “idle is a fallback, not a peer”; invariant rejects PID 0 in a queue |
| **`SYS_EXIT` terminates** | ✅ Prints `[Process exited]`, parks the caller as a zombie, returns to idle | interactive QEMU test |
| **Full kernel build/link** | ✅ `tools/build_kernel.py` compiles all `kernel/*.c` + `lib/*.c` + asm, links, and embeds the ring-3 shell | `build.bat`, `build.py`, `rebuild.bat` all pass |
| **Boot image validation** | ✅ `os.elf` is ELF32, has a valid multiboot header, and contains the exact kernel blob at `0x200000` | build script verifies both before reporting success |
| **Boot stack + blob retention** | ✅ `boot32.S` loads `%esp` from `.bss`; `KEEP()` + `--no-gc-sections` stop LLD dropping the kernel blob | `readelf`-level inspection: PT_LOAD at 0x200000 present |
| **Freestanding libc core** | ✅ `memset`, `memcpy`, `memmove`, `strlen`, `strcmp`; `malloc`/`free` wrap `kmalloc`/`kfree` | `lib/string.c`, `lib/stdlib.c` compiled into the kernel |
| **COM1 receive path** | ✅ IRQ4 → vector 36, level-triggered IOAPIC route, 64-byte software ring, 14-byte FIFO trigger + receive-timeout for short lines | runtime `help`, `clear`, `touch runtime.txt`, `ls`, `exit` all complete correctly |
| **Boot + interactive ring-3 runtime** | ✅ Long mode, memory, ACPI/LAPIC/IOAPIC, heap self-test, ring-3 shell, serial input | `tools/boot_test.py`: `arc> ` prompt, command output, `BOOT OK` |
| **SSE-free codegen** | ✅ Kernel/user code builds with `-mno-sse -mno-sse2 -mno-mmx -mno-avx`; build rejects vector instructions | build-time `objdump` guard (`NO-SSE`) |
| **Per-process POSIX fd layer** | ✅ Per-process fd tables over shared open file descriptions, `dup`, deferred unlink, access/seek checks; `echo text to file` now overwrites (truncates) | 164 fd + 37 fs host checks; full gate green; QEMU `cat fdtest.txt` → `beta` |

### ❌ Still Missing

- Automatic reaping (idle reaper): ✅ process 0's idle loop now reaps zombies and frees their owned frames. Parent-aware `wait()` is still missing.
- `fork`/`exec`/`wait`. The POSIX file-descriptor table is now implemented (per-process fds over shared open file descriptions); a future `fork` can clone it via `fd_clone_process()`.
- FAT32 and on-disk filesystems: the kernel has an ATA PIO driver but only a RAM-backed FS.
- Demand paging, copy-on-write, and recoverable page faults.
- SMP: per-CPU slab slots exist but the kernel is effectively UP-only.
- Serial hardware flow control (RTS/CTS); TCP backpressure protects the scripted tests, but a real UART can overrun under extreme bursts.

### ⚠️ Known Limitations (next milestones)

| Limitation | Impact | Next step |
|------------|--------|-----------|
| SSE/MMX disabled until the context switch saves XMM state | Moderate – no FP in kernel; enabling SSE needs FXSAVE | keep the build guard; add FXSAVE before enabling SSE |
| User frames leak when a process exits | Resolved – `owned[]` frames are freed on reap | keep the atomic alloc+attach API for new resource types |
| Loader publish window | Low today – `preempt_disable()` keeps a half-mapped process off the CPU | use an explicit deferred-start API if setup grows beyond a few steps |
| Zombies need an explicit reap; no `wait()` | Resolved for resource leaks | idle-loop reaper frees frames immediately; parent-aware `wait()` waits for fork/exec |
| `scheduler_boost()` rebuilds all queues | Low today – bounded by `MAX_PROCESSES` (16) | use per-level queues only if the table grows |
| SMP: per-CPU slots exist but ownership is UP-only | Low today, high when SMP starts | revisit at SMP bring-up |
| FAT32 driver not integrated with slab | Moderate | Phase 3 filesystem work |
| COM1 RX has no RTS/CTS | Low at 115200 with a 64-byte ring | add flow control only if real-hardware bursts overrun |

### 🧪 Verification Status

| Test | Result | Method |
|------|--------|--------|
| Slab/memory host stress | ✅ 2269 checks, 0 failures | `cmd /c tests\run_tests.bat` |
| Scheduler host tests | ✅ 343 checks, 0 failures (includes idle reaper) | `cmd /c tests\run_tests.bat` |
| POSIX fd host tests | ✅ 164 checks, 0 failures | `cmd /c tests\run_tests.bat` |
| Filesystem slot host tests | ✅ 37 checks, 0 failures | `cmd /c tests\run_tests.bat` |
| POSIX fd boot gate | ✅ QEMU `cat fdtest.txt` → `beta`; `--expect-not alphabeta` and `--expect-not betaa`; `BOOT OK: 1675 bytes` | `cmd /c tests\run_all.bat` |
| Test sensitivity (mutation) | ✅ 3 failures injected, 3 detected | mutated double-free check |
| Full kernel build/link | ✅ 201,976-byte kernel ELF, 44,384-byte raw kernel | `python tools\build_kernel.py --quiet` |
| Boot image | ✅ 66,868-byte `os.elf`, multiboot OK, kernel bytes verified at 0x200000 | build-script validation |
| No vector instructions | ✅ `NO-SSE` build check passes | `objdump` scan in build script |
| QEMU interactive shell | ✅ `help`, `clear`, `touch runtime.txt`, `ls`, `exit`; 1,528 bytes of serial output | `python tools\boot_test.py ...` over TCP serial |
| Boot race regression | ✅ 5/5 additional boots `BOOT OK` after the loader fix | repeated `python tools\boot_test.py` runs |
| Combined quality gate | ✅ host tests → build → interactive boot test all pass | `cmd /c tests\run_all.bat` |

### 🎯 Immediate Next Session

| Goal | Tasks |
|------|-------|
| Automatic reaping (idle reaper) | ✅ Done: idle loop reaps zombies so owned frames are released at runtime |
| Filesystem/POSIX foundation | ✅ Done: POSIX fd table, open/read/write/close/dup semantics with tests; FAT32 read path still pending |
| Re-run gates | Host tests + full build + interactive boot after each thin slice |

---

### Active Slice: Automatic Zombie Reaping (2026-11-20)

**Spec:** `docs/specs/automatic-zombie-reaping.md`

#### Overview

Add one scheduler API, `scheduler_reap_zombies()`, and call it from process 0's
idle loop. It reaps every zombie except the current process, using the existing
`scheduler_reap()` path so frame freeing stays in one place. Parent-aware
`wait()` is intentionally out of scope.

#### Architecture Decisions

- The idle loop is the natural reaper: it already owns no user resources and
  runs whenever no real process is runnable.
- Reaping stays a scheduler concern; `kernel.c` only calls the API.
- Preemption is disabled for the whole scan so a timer tick cannot observe a
  half-reaped process table.
- Returning a count (not a pid) keeps the interface honest: there is no parent
  relationship or exit-status delivery yet.

#### Task List

- [x] Task AR-1: Write a failing host test for idle reaping
- [x] Task AR-2: Implement `scheduler_reap_zombies()` and idle-loop call
- [x] Task AR-3: Run host tests, full build, and interactive boot gate

#### Checkpoint: Automatic Reaping Complete

- [x] Host scheduler test proves net-zero PMM frames without explicit reap
- [x] `cmd /c tests\run_tests.bat` passes with 0 failures (343 checks)
- [x] `cmd /c tests\run_all.bat` passes, including `exit` and `BOOT OK`

#### Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Timer tick observes a partly reaped process | Medium | `preempt_disable()` around the scan |
| Reaper steals a zombie another caller wants | Low today | No parent-aware wait exists; documented in the spec |
| Idle loop calls the scanner too often | Low | 16-entry scan, no allocation, one `hlt` per iteration |

---

### Active Slice: POSIX Descriptor Layer (2026-11-20)

**Spec:** `docs/specs/posix-fd-layer-design.md`

#### Overview

Per-process fd tables point to shared open file descriptions (OFDs). Each
`open()` creates one OFD holding the file slot, offset, flags and a refcount;
`dup()` and a future `fork()` share the OFD so they share one offset. Closing
the last fd for an OFD releases its single file-slot reference. Deleting an open
file defers freeing its blocks until the last descriptor closes.

#### Architecture Decisions

- Fds are per-process and validated against that process's table; 0/1/2 are
  standard in/out/err for every process.
- The OFD owns exactly one slot reference: `fs_open_slot()` takes no ref,
  `fd_open()` adds one, `dup`/clone bump only OFD refs, release at refs 0.
- `FS_DISK_VERSION` is 2; older disk images are reformatted.
- `SYS_DUP` is 13; `SYS_OPEN` takes flags and `SYS_SEEK` takes `whence`.
- Access flags and seek bounds (`[0, size]`, no sparse files) are enforced at
  the fd/syscall boundary; fd/OFD pools are fixed size (no dynamic allocation).

#### Task List

- [x] Per-process std fd table and fd host suite
- [x] Shared open file descriptions, `dup`, refcount and pool-exhaustion tests
- [x] Filesystem slot API with deferred unlink and real-fs host suite
- [x] `--expect-not` boot-gate assertions for overwrite semantics
- [x] Switch kernel/shell to POSIX fd syscalls; remove stale fd-shaped fs API
- [x] Design doc status and verification evidence; full final gate

#### Checkpoint: POSIX Descriptor Layer Complete

- [x] Host suites green: 2269 + 343 + 164 + 37 checks, 0 failures
- [x] Full build and `NO-SSE` build guard pass
- [x] `cmd /c tests\run_all.bat` passes: QEMU `cat fdtest.txt` → `beta` with
      `--expect-not alphabeta` and `--expect-not betaa`; `BOOT OK: 1675 bytes`

#### Behaviour change

`echo text to file` now opens with `O_WRONLY | O_CREAT | O_TRUNC` and
**overwrites (truncates)** the target instead of appending.

#### Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| ABI flag change breaks the shell | High | All `SYS_OPEN` call sites updated; boot gate covers echo/cat/touch/cp paths |
| Refcount bug leaks or double-frees a slot | High | Host tests assert one slot ref per OFD and net-zero after close-all |
| Disk layout change loses existing files | Medium | `FS_DISK_VERSION` 2; older images reformatted |

---

## 🛠️ Build & Test Commands (Quick Reference)

```cmd
REM Full build (native Windows toolchain, no MSYS2 shell)
cmd /c C:\Users\wadia\ARC_os\build.bat

REM Same build via Python
python C:\Users\wadia\ARC_os\build.py

REM Clean rebuild
cmd /c C:\Users\wadia\ARC_os\rebuild.bat

REM Reproduce both host suites (slab/memory + scheduler)
cmd /c C:\Users\wadia\ARC_os\tests\run_tests.bat

REM Full gate: host tests + build + interactive QEMU shell test
cmd /c C:\Users\wadia\ARC_os\tests\run_all.bat

REM Run interactively (typed input over COM1 stdio)
qemu-system-x86_64 -kernel C:\Users\wadia\ARC_os\os.elf -nographic -m 512M
```