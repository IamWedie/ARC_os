# Spec: Per-Process Address Spaces

**Status:** Proposed (controller decisions delegated; rulings recorded below)
**Scope:** first module of the process-lifecycle initiative — one page-table
root per user process, shared kernel mappings, CR3 on context switch, and
reclaim on reap. No `fork`, `exec`, `wait`, or copy-on-write.

## Problem

The kernel has exactly one page table. `kernel/vmm.c:18-23` defines a single
`vmm_pml4`, `vmm_pdpt`, and `vmm_pd[]`; `vmm_init()` loads it into CR3 once
(`kernel/vmm.c:67`). `vmm_map_page()` and `vmm_unmap_page()` always walk that
global root (`kernel/vmm.c:77,108`), and `vmm_create_user_mapping()` maps the
ring-3 shell at fixed VAs into it (`kernel/vmm.c:120-124`, called from
`kernel/user.c:37,43`). Context switches in `schedule()` only swap stack
pointers (`kernel/scheduler.c:293-371`); CR3 is never reloaded.

Consequence: two user processes at `USER_CODE_VA` (`0x8000000000`) and
`USER_STACK_VA` (`0x9000000000`) would overwrite the same PTEs. A forked child
resuming at the parent's RIP would corrupt the parent's code and stack. No
amount of trap-frame plumbing in a later `fork` module can fix that; the
address-space foundation must land first.

The current kernel's VA layout is unusual but load-bearing:

| VA region | PML4 slot | Flags | Owner |
|---|---|---|---|
| Identity map of RAM, kernel text, kernel stacks, MMIO | 0 | supervisor `PRESENT|RW` (2 MiB and 4 KiB) | shared |
| User code `0x8000000000` | 256 | `PRESENT|RW|USER` (4 KiB) | private |
| User stack `0x9000000000` | 288 | `PRESENT|RW|USER` (4 KiB) | private |

## Goals

1. Every user process has its own PML4. Kernel mappings are shared by copying
   the supervisor slots of the master root; user slots start empty.
2. The correct root is loaded into CR3 on every actual process switch and on
   fallback to the idle process.
3. User code/stack mappings are installed in the process's own root, not the
   global one.
4. Reap frees the private root and every private page-table page exactly once,
   without touching shared kernel tables or user *data* frames (already owned
   via `owned[]`).
5. Page-table construction, lookup, clone, and teardown are pure C in a
   host-testable module with injected allocation, so the isolation property is
   proven by tests rather than by boot observation alone.
6. Existing behaviour is unchanged: one shell, same fixed VAs, same gate.

## Non-goals

- `fork`, `exec`, `wait`, parent/child links, exit-status delivery.
- Copy-on-write, page-fault handling, demand paging, ASLR, NX bits.
- PCID/ASID, SMP, per-CPU page tables, 5-level paging, large user pages.
- Moving the kernel to the canonical upper half (would touch boot, linker,
  and every MMIO mapper for no benefit to this module).
- Changing fd semantics, MLFQ policy, or the user binary format.

## Design rulings

These are the decisions made under delegated authority. Each is recorded with
the cost of getting it wrong so a later reviewer can challenge it.

- **A1 — Private root per user process; master root for PID 0 and kernel
  threads.** `process_t` gains `uint64_t pml4_phys`; zero means "use master".
  *Why:* kernel threads have no user mappings and must see the one kernel
  image. *Cost if wrong:* giving kernel threads empty roots breaks their
  stacks and trap frames; giving them private copies wastes frames without
  adding isolation.

- **A2 — Share PML4 slots [0,256), keep [256,512) private.** The clone copies
  the first 256 master entries verbatim and zeroes the rest. Those 256 slots
  contain every kernel identity map and MMIO mapping (`vmm_map_2mb()` rejects
  `pml4i != 0`; ACPI/LAPIC/IOAPIC map 4 KiB pages in the same slot).
  *Cost if wrong:* copying user slots shares page tables between processes
  (the original bug); dropping slot 0 makes the kernel invisible after CR3
  load and faults on the first `iretq`.

- **A3 — Track private page tables by walking the root, not in `process_t`.**
  `pt_free_user_tables()` recursively frees tables reachable from user slots;
  `as_destroy()` then frees the root page itself. No new fixed-size array on
  the process. *Cost if wrong:* a parallel array can drift from the tables the
  hardware actually uses and either leaks frames or frees a shared table.

- **A4 — Pure core with injected allocator.** A new `kernel/pagetable.c`
  implements map/lookup/unmap/clone/free against a caller-supplied root and
  `pt_ops_t` allocator. All inline assembly (`invlpg`, `mov cr3`) stays in
  `kernel/vmm.c`. *Cost if wrong:* testing page tables inside the real VMM
  would require executing privileged instructions on the host, so isolation
  would only ever be verified by a boot smoke test.

- **A5 — Keep user VAs fixed.** No relocation or ASLR in this module; `fork`
  relies on parent and child sharing the same VAs.
  *Cost if wrong:* adding per-process VA bases now would force changes to the
  embedded shell's link addresses and complicate the fork RIP clone.

- **A6 — Eager, not COW.** The address-space API maps data frames supplied by
  the caller. Sharing/copying those frames is a later `fork` concern.
  *Cost if wrong:* implementing faults now would couple this module to
  `exception_handler()` (which currently halts on any fault) and to PMM
  refcounts, neither of which exists.

- **A7 — Full CR3 reload, no PCID.** Every real switch writes CR3; the TLB
  flush is acceptable on a uniprocessor kernel.
  *Cost if wrong:* PCID management needs `CR4.PCIDE`, INVPCID fallback paths,
  and SMP-safe ASID allocation — all out of scope.

## Interface contract

```c
/* kernel.h — page-table constants and injected allocator */
#define PT_PG_PRESENT (1ULL << 0)
#define PT_PG_RW      (1ULL << 1)
#define PT_PG_USER    (1ULL << 2)
#define PT_PG_PSE     (1ULL << 7)
#define PT_USER_SLOT_FIRST 256       /* PML4 slots [0,256) are shared kernel */

typedef struct {
    void* (*alloc)(void);            /* zeroed 4096-byte page, or NULL */
    void  (*free)(void*);
} pt_ops_t;

/* kernel/pagetable.c — pure, no asm, no PMM dependency */
int      pt_map(uint64_t* root, uint64_t vaddr, uint64_t paddr,
                uint8_t flags, const pt_ops_t* ops);
uint64_t* pt_lookup(uint64_t* root, uint64_t vaddr);
int      pt_unmap(uint64_t* root, uint64_t vaddr);
void     pt_clone_shared(uint64_t* src, uint64_t* dst);
void     pt_free_user_tables(uint64_t* root, const pt_ops_t* ops);
int      pt_count_user_pages(const uint64_t* root);   /* test introspection */

/* kernel/vmm.c — master table and hardware glue */
uint64_t vmm_master_root(void);
void     vmm_invalidate_page(uint64_t vaddr);
void     vmm_switch_address_space(int pid);

/* kernel/address_space.c — process-facing service */
int  as_create(int pid);
int  as_map_user(int pid, uint64_t vaddr, uint64_t paddr,
                 size_t pages, uint8_t flags);
void as_destroy(int pid);
uint64_t as_root(int pid);            /* 0 when the process has no private root */
```

`process_t` gains one field: `uint64_t pml4_phys` (0 = master). It is zeroed by
`scheduler_init()` and by `process_build_common()` through the existing
`(process_t){0}`-style initialization, and cleared by `as_destroy()`.

### Invariants

- `pt_map()` creates each intermediate table at most once; flags `0` mean
  `PT_PG_PRESENT|PT_PG_RW` (preserves today's MMIO mapper behaviour). A large
  page encountered on the walk is refused, not split.
- `pt_lookup()` returns `NULL` for any absent level or large-page path.
- `pt_clone_shared()` copies [0,256) and zeroes [256,512). It never allocates.
- `pt_free_user_tables()` frees only pages reachable from slots >= 256 and
  never frees the root. It zeroes those root slots when done.
- `as_create()` rejects `pid <= 0`, out-of-range pids, and a process that
  already has a root. On allocation failure the process keeps `pml4_phys == 0`.
- `as_map_user()` requires a private root and ORs `PT_PG_USER` into every leaf
  and intermediate entry. A partial failure leaves valid partial mappings;
  the caller's kill+reap path owns cleanup.
- `scheduler_reap()` calls `as_destroy()` before clearing the rest of the
  process. `as_destroy()` is idempotent (zeroes `pml4_phys`).
- `vmm_switch_address_space(pid)` loads the master root for `pid <= 0`, for
  `pid >= MAX_PROCESSES`, and for any process with `pml4_phys == 0`.

## File responsibilities

| File | Change |
|---|---|
| `kernel/pagetable.c` | new — pure 4-level walk/map/unmap/clone/free |
| `kernel/address_space.c` | new — per-process roots via PMM and `pt_ops_t` |
| `kernel/vmm.c` | use pure core for master `map_page`/`unmap_page`; export master root, invalidate, and CR3 switch; drop `vmm_create_user_mapping` |
| `kernel/user.c` | call `as_create()` and `as_map_user()` instead of the global mapper |
| `kernel/scheduler.c` | call `vmm_switch_address_space()` on real switches and idle fallback; call `as_destroy()` in reap |
| `kernel/kernel.h` | constants, `pt_ops_t`, `pml4_phys`, prototypes |
| `tests/test_pagetable.c` + `tests/pt_stub.c` | new — pure-core host suite |
| `tests/test_address_space.c` | new — process-facing suite on host stubs |
| `tests/test_scheduler.c` / `tests/scheduler_stub.c` | assert reap calls `as_destroy`; stub CR3 switch and address-space teardown |
| `tests/run_tests.bat` | add the two new suites |
| `tests/run_all.bat` | unchanged gate; new suites join `[1/3]` |

## Acceptance criteria

- `cmd /c tests\run_tests.bat`: all six existing suites unchanged in count (at
  least slab 2269, scheduler 343, fd 194, fs-slot 38) plus the two new suites;
  0 failures. New tests prove:
  - a mapping in process A's root is invisible in process B's root;
  - cloned roots share slot 0 but have zeroed user slots;
  - `pt_free_user_tables()` frees exactly the private tables and leaves shared
    tables untouched;
  - repeated create → map → kill → reap cycles return the PMM stub to its
    starting page count;
  - reap invokes `as_destroy()` exactly once per reaped process.
- `cmd /c tests\run_all.bat`: full gate green. Build still passes `NO-SSE`.
  QEMU boot test still reaches `arc> ` and completes the same command script
  (`cat fdtest.txt` → `beta`, no `alphabeta`/`betaa`, `BOOT OK`).
- No change to scheduler policy counters, fd behaviour, syscall numbers, or
  the embedded shell image.

## Risks / mitigations

| Risk | Mitigation |
|---|---|
| CR3 switched while the current stack is not mapped in the new root | All kernel stacks and handler code are in shared slot 0; switch happens on the current kernel stack, which remains mapped. |
| Reap frees a shared table | Teardown walks only user slots (>= 256); shared slot 0 is never traversed. |
| Partial `as_map_user()` failure leaks tables | Loader keeps its `preempt_disable()` window and uses kill+reap; `as_destroy()` frees the partial root. |
| Boot-time MMIO mappers run before a process root exists | They call `vmm_map_page()` on the master root, unchanged. |
| Host tests diverge from kernel flags | Both use the same `kernel/pagetable.c`; only allocator and invalidate hook differ. |
| A second user process is spawned before this module is complete | Not possible today: only `user_process_create_from_blob()` creates user processes, and it uses the new API. |

## Verification evidence

Pending implementation. Fill in RED/GREEN counts, build sizes, and QEMU result
after the module lands.
