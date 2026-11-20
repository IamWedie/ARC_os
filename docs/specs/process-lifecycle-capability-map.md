# Capability Map: Process Lifecycle (fork / exec / wait)

Status: Approved by controller delegation (human partner said "do what you gotta do man").

## Why this map exists

`fork`/`exec`/`wait` are not one feature. Three independently testable capabilities
have to exist before a POSIX-style process lifecycle works, and the current kernel
has none of their prerequisites: it has a single global page table, fixed user
virtual addresses, no parent/child relationship, and only one embedded flat
user binary. This map fixes module boundaries and build order before any module
spec is written, so a later module cannot silently depend on an earlier one's
implementation details.

## Modules

| Module id | Responsibility | Depends on |
|---|---|---|
| `address-space` | One page-table root per process; kernel mappings shared; per-process user code/stack mappings; CR3 loaded on context switch; page tables freed on reap. | — (builds on existing PMM/VMM/scheduler) |
| `fork-wait` | `SYS_FORK` (clone address space, capture user trap frame, `fd_clone_process`, parent/child links), `SYS_WAIT` (block parent, consume child exit code), parent-aware reaping so the idle reaper cannot destroy a zombie before its parent observes it. | `address-space`, fd layer (landed) |
| `exec-loader` | Load a program image from the filesystem into a fresh address space, argument passing ABI, close-on-exec, and a build pipeline that can produce more than one user binary. | `address-space`, `fork-wait` |

Build order: `address-space` → `fork-wait` → `exec-loader`.

## Boundary rules

- A module owns its kernel state. `fork-wait` may call the published `address-space`
  API but must not reach into its page-table internals. `exec-loader` may call
  both published APIs but must not edit `process_t` layout.
- Each module gets its own spec, plan, task list, review, and gate before the next
  module starts. A module is not "done" until the full gate is green on the merged
  result.
- No module changes scheduler policy or MLFQ counters. No module changes the fd
  refcount contract (one slot ref per OFD) or existing syscall numbers.
- UP-only assumptions stay UP-only; SMP is explicitly out of scope for all three.

## Controller decisions (delegated)

- **C1** — `address-space` is first because a forked child executes the same code
  at the same virtual addresses; without per-process page tables it would
  overwrite the parent. Cost if wrong: fork silently corrupts the parent and
  no amount of trap-frame plumbing fixes it.
- **C2** — `fork-wait` is second because `fd_clone_process()` already exists and
  is untested in production; wiring it through a real `fork` gives it a real
  caller before `exec-loader` complicates the picture. Cost if wrong: an unused
  fd-clone path rots and the refcount contract drifts.
- **C3** — `exec-loader` is last and is a separate initiative-sized module; it
  needs a disk-backed program format and a multi-binary build pipeline, which
  are larger than either preceding module. Cost if wrong: trying to bundle it
  with `fork` produces a half-built loader and an unverifiable gate.
- **C4** — This session delivers the `address-space` module only. Later modules
  are re-scoped against the real result of this one. Cost if wrong: planning
  detail for modules 2 and 3 is premature and will be redone.
