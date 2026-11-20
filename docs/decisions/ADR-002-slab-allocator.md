# ADR-002: Size-classed slab allocator with a PMM fallback

## Status
Accepted

## Date
2026-11-18

## Context

The kernel needs a fast, predictable heap for small objects (process metadata,
filesystem structures, future GUI objects) while still supporting occasional
large allocations. The previous implementation was broken: duplicate cache
definitions, metadata overlapping objects, `kfree` always using class 0, and
no working double-free detection.

## Decision

Use 12 power-of-two size classes from 16 B to 32 KiB:

- Each small slab occupies exactly one 4 KiB page.
- Slab metadata lives at the page base; objects follow it.
- Each CPU has an active slab per class; partially used slabs sit on a global
  partial list.
- A per-slab allocation bitmap detects double frees and invalid pointers.
- Requests larger than 32 KiB bypass the slab and allocate a PMM page run with
  a header at the base.

`kmalloc`/`kfree` are the kernel API. `malloc`/`free` are thin wrappers for
code that expects C runtime names.

## Alternatives Considered

### Buddy allocator for every allocation
- Pros: Simple single mechanism; good large-allocation behavior.
- Cons: Internal fragmentation for small objects; slower common path.
- Rejected: Small objects dominate expected kernel usage.

### Full dlmalloc-style allocator
- Pros: Mature, general-purpose.
- Cons: Large surface area, harder to verify, unnecessary for this workload.
- Rejected: Too much complexity for the current stage.

### Per-class magazine caches
- Pros: Lower contention on SMP.
- Cons: More states to validate; the kernel is effectively UP today.
- Deferred: Per-CPU active slabs are the current boundary; magazines can be
  added when SMP bring-up starts.

## Consequences

- Allocation is O(1) in the common case (pop from the active slab free list).
- The host stress test links the real `slab.c` and `memory.c` and validates
  11,841 allocations, boundary sizes, double frees, and PMM fallback.
- Slab ownership is currently UP-only even though per-CPU slots exist.
- Objects larger than 32 KiB pay for a page-run header and PMM search.
