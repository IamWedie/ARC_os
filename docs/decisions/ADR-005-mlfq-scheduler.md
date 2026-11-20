# ADR-005: Four-level MLFQ scheduler with explicit process states

## Status
Accepted

## Date
2026-11-20

## Context

The original scheduler kept a single `running` flag per process and picked the
next PID with a linear scan. It had no way to block or wake a process, no
voluntary yield, no statistics, and `SYS_EXIT` only printed a message. The
idle loop also competed with runnable work for CPU time, so an interactive
shell could be starved by a busy process.

The kernel is UP-only and supports at most 16 processes. The scheduler needs to
be predictable, easy to test on the host, and good enough for interactive work
without importing the complexity of a general-purpose fair scheduler.

## Decision

Use a four-level multi-level feedback queue (MLFQ):

- Level 0 is the highest priority; level 3 is the lowest.
- Each level is a fixed-size FIFO ring (`pid[MAX_PROCESSES]`, `head`, `count`).
  Push and pop are O(1). Removing a blocked/killed process is O(n) but bounded
  by 16 entries.
- A running process is charged one tick per timer interrupt. After
  `SCHED_QUANTUM_TICKS` (10 ticks at 100 Hz = 100 ms) it is demoted one level.
- Every `SCHED_BOOST_TICKS` (100 ticks = 1 s) all ready/running processes return
  to level 0. This bounds starvation of interactive work.
- The scheduler picks the first non-empty level and pops its FIFO head.

Process states are explicit:

```
FREE -> READY -> RUNNING -> { READY, BLOCKED, ZOMBIE } -> FREE
```

`PROC_IDLE` is granted to PID 0 only. PID 0 is never placed in a run queue and
is never a scheduling candidate while any real process is ready. It cannot be
blocked, killed, or reaped. If no real process is runnable, the scheduler stays
on the idle stack and the CPU halts until the next interrupt.

Voluntary switches use a software interrupt (`int $0x81`) rather than waiting
for the next timer tick. The assembly stub saves the general-purpose registers,
calls `scheduler_yield_handler(rsp)`, and switches to the returned stack
pointer if it is non-zero. A yield resets the current slice but does not demote
the process; a timer preemption charges the quantum and may demote.

The TSS `RSP0` is updated to the incoming process's kernel stack on every
switch so ring-3 interrupts and syscalls land on the right kernel stack.

## Alternatives Considered

### Keep a single round-robin queue
- Pros: Simplest possible implementation; no demotion or boost logic.
- Cons: No distinction between interactive and CPU-bound work; no block/wake
  semantics; idle remains a peer and can steal time.
- Rejected: It cannot meet the "interactive shell stays responsive" goal.

### MLFQ without periodic boost
- Pros: Less bookkeeping.
- Cons: A CPU-bound process sinks to level 3 and can starve if new interactive
  work keeps arriving at higher levels (or, conversely, a hog at level 0 is
  never displaced).
- Rejected: The 100-tick boost is cheap at this scale and bounds starvation.

### CFS/EEVDF-style fair scheduling
- Pros: Strong fairness and latency guarantees; scales beyond 16 processes.
- Cons: Requires a balanced tree or runqueue with virtual-time accounting,
  much more state to validate, and is overkill for the current process count.
- Deferred: Revisit if the process table grows or SMP arrives.

### Keep idle as the lowest-priority queued process
- Pros: One uniform scheduling path.
- Cons: Idle can be picked while other processes are momentarily blocked,
  consumes a quantum, and complicates demotion/boost accounting. It also makes
  it possible for idle to be "ready" alongside real work.
- Rejected: The explicit `PROC_IDLE` fallback is simpler to reason about and
  makes the invariant "idle is never queued" directly testable.

### Timer-only preemption (no yield vector)
- Pros: No extra IDT gate or assembly stub.
- Cons: A syscall that blocks would wait up to a full quantum; exit/block
  latency becomes 10 ms.
- Rejected: The software yield vector gives immediate switches and a clean
  host-testable path.

## Consequences

- Interactive work runs within one 100 ms quantum; the periodic boost prevents
  indefinite starvation.
- The host test links the real `scheduler.c` and checks states, FIFO fairness,
  demotion, boost, block/wake, kill/reap, idle fallback, and invariants
  (251 checks).
- Queue storage is a fixed 16 bytes per level, so the O(1) claim has a tiny
  memory cost and no dynamic allocation.
- `scheduler_reap()` currently frees only the kernel stack. User code and user
  stack frames are not owned by `process_t` yet, so repeated user-process
  creation leaks them. Process resource ownership is the next scheduler task.
- Zombies require an explicit reap; there is no `wait()` yet.
- The design is UP-only: `current_process` and the run queues are global. SMP
  will need per-CPU queues and locking, or a redesign.
