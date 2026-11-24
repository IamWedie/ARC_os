# M0.5 §2 identity-map gap — PMM allocation range vs VMM mapped range

Status: **PROPOSED — no fix implemented, no kernel/test code changed.**
This note addresses contract rule 12: the frozen assumption is *"PMM may hand out any
4 KiB frame inside an AVAILABLE region, and VMM guarantees that frame is reachable
through the identity map."* That assumption is false. Approval is required before
either side of it changes.

## 1. The two contracts, from the real code

- `kernel/pmm.c:93-104` stores each AVAILABLE region verbatim (`e->base`, `e->len`)
  in `pmm_regions[]`. Only AVAILABLE entries are recorded; reserved entries are
  used solely to re-reserve frames in `kernel/pmm.c:106-111`.
- `kernel/pmm.c:47-62` `pmm_release(base, len)` starts at `f0 = base >> 12`, i.e. it
  frees frames from the **verbatim 4 KiB-aligned region base**, not from a 2 MiB
  boundary.
- `kernel/pmm.c:132-151` `pmm_alloc_pages()` returns the first free frame, so the
  base frame of an unaligned AVAILABLE region is allocatable.
- `kernel/vmm.c:67-80` maps `pmm_regions[]` with 2 MiB pages, but starts the loop at
  `start = (r.base + 0x1FFFFF) & ~0x1FFFFFULL` (round **up**) and stops at
  `a < claimed_end`.
- `kernel/vmm.c:59-60` unconditionally maps only `[0, 2 MiB)`.

The gap: frames in `[r.base, round_up_2MiB(r.base))` are allocatable but not mapped
by the region loop. They are only safe when they happen to fall below 2 MiB.

Boot-order facts (relevant to candidate-fix feasibility): `kernel/kernel.c:30-31`
calls `pmm_init(&bi)` then `vmm_init()` with no allocation in between, and CR3 is
loaded at `kernel/vmm.c:83`. `vmm_alloc_table()` (`kernel/vmm.c:30-34`) is reachable
through `vmm_ops` only from `vmm_map_page()` (`kernel/vmm.c:86-88`), **not** from
`vmm_init()`. So `vmm_init()` does **not** allocate page tables from PMM; there is no
circular dependency that forbids a check inside `pmm_alloc_pages()`.

Two related facts the fix must not overstate:

- `kernel/vmm.c:62-66` comments that reserved regions are mapped too, but
  `pmm_regions[]` contains only AVAILABLE entries, so the loop never sees reserved
  entries. In the captured real map below, the reserved top block
  `[0x1FFE0000, 0x20000000)` is nevertheless reachable because the AVAILABLE
  region's partial tail page (`0x1FE00000`) spans it. That coverage is incidental;
  it is not a second region loop. Reserved-region mapping is a separate,
  pre-existing documentation/implementation mismatch and is out of scope here.
- `kernel/vmm.c:51` masks the physical address with `0xFFE00000ULL`, which keeps
  only bits 21-31. Any 2 MiB-aligned PA at or above 4 GiB is therefore mapped to the
  wrong physical page, while `kernel/pmm.c:13` allows PMM to allocate up to 16 GiB.
  The captured firmware map even contains a reserved entry above 4 GiB (§3), so
  high physical addresses are real, not hypothetical. This is a **separate defect**:
  the start-rounding fix below does not correct it, and no claim of full 16 GiB
  identity-map correctness is made here.

## 2. Concrete proof (host test, synthetic map)

The test below links the **real `kernel/pmm.c`**. The two link-time kernel-image
symbols are supplied by host arrays; MinGW places them near its image base, which
may lie inside PMM's 16 GiB window, so `pmm_init()` can reserve a few host frames.
The assertions confirm none of those frames fall inside the synthetic region — the
proof does not depend on where the host linker puts the stub.

Only the arithmetic of `kernel/vmm.c:16`, `:51`, `:60`, and `:71-80` is mirrored;
no live kernel is faulted.

Synthetic memory map: one AVAILABLE region `[0x300000, 0x400000)` — 1 MiB at 3 MiB,
4 KiB-aligned but not 2 MiB-aligned.

Arithmetic:

```text
PMM:    pmm_release(0x300000, 0x100000) -> frame 0x300 is free
VMM:    claimed_end = 0x400000
        start = (0x300000 + 0x1FFFFF) & ~0x1FFFFF = 0x400000
        for (a = 0x400000; a < 0x400000; a += 0x200000) -> zero iterations
Result: pmm_alloc_pages() returns 0x300000; its 2 MiB page (0x200000) has no
        identity-map entry. Any direct dereference of that PA faults.
```

Test source (kept outside the working tree; this note is the only deliverable):

```c
/* Throwaway host proof: PMM can hand out a frame VMM does not identity-map. */
#include <stdint.h>
#include <stdio.h>
#undef SYS_OPEN
#include "kernel.h"
#include "bootinfo.h"

/* pmm.c resolves these at link time; MinGW places them near its image base.
 * They are ordinary host addresses, so pmm_init() may reserve a few host frames;
 * the CHECKs below confirm none of them fall inside the synthetic region. */
unsigned char __kernel_start[16];
unsigned char __kernel_end[16];

#define TWO_MB 0x200000ULL
#define PAGE_2MB(pa) ((pa) & ~(TWO_MB - 1))
#define VMM_PDE_ADDR_MASK 0xFFE00000ULL /* kernel/vmm.c:51 */
#define VMM_PD_COUNT 128                /* kernel/vmm.c:16,50 */

static int run, fail;
#define CHECK(c, m) do { run++; if (!(c)) { fail++; printf("FAIL: %s\n", m); } } while (0)

/* Exact expression from kernel/vmm.c:71. */
static uint64_t vmm_rounded_start(uint64_t base) {
    return (base + 0x1FFFFF) & ~0x1FFFFFULL;
}

/* Region-loop portion of vmm.c:76-80, including the vmm_map_2mb() guard. */
static int vmm_loop_maps_frame(uint64_t start, uint64_t claimed_end, uint64_t frame) {
    uint64_t page = PAGE_2MB(frame);
    uint64_t pdpti = (page >> 30) & 0x1FF;
    uint64_t pml4i = (page >> 39) & 0x1FF;
    if (pml4i != 0 || pdpti >= VMM_PD_COUNT) return 0;
    for (uint64_t a = start; a < claimed_end && a >= start; a += TWO_MB)
        if (a == page) return 1;
    return 0;
}

static struct bootinfo make_map(uint64_t base, uint64_t len) {
    struct bootinfo bi = {0};
    bi.flags = BOOTINFO_FLAG_MMAP;
    bi.mmap_count = 1;
    bi.mmap[0] = (struct bootinfo_mmap_entry){base, len, BOOTINFO_MMAP_AVAILABLE, 0};
    return bi;
}

int main(void) {
    struct bootinfo bi = make_map(0x300000ULL, 0x100000ULL);
    struct bootinfo real_bi;
    void* frame;
    uint64_t start_up, start_down, real_end;

    pmm_init(&bi); /* real PMM: pmm_release() frees from the verbatim base */
    CHECK(pmm_region_count_get() == 1, "PMM recorded one available region");
    CHECK(pmm_region_get(0).base == 0x300000ULL, "PMM region base retained verbatim");

    frame = pmm_alloc_pages(1); /* real PMM allocator */
    CHECK(frame == (void*)0x300000ULL, "PMM hands out the unaligned-region frame");

    start_up = vmm_rounded_start(0x300000ULL);
    start_down = 0x300000ULL & ~(TWO_MB - 1);
    CHECK(start_up == 0x400000ULL, "vmm_init rounds region start up to 4 MiB");
    CHECK(!vmm_loop_maps_frame(start_up, 0x400000ULL, (uint64_t)frame),
          "rounded-up start leaves the PMM frame unmapped");
    CHECK(vmm_loop_maps_frame(start_down, 0x400000ULL, (uint64_t)frame),
          "rounded-down start would cover the same frame");

    /* Control: real QEMU -m 512M MB1 map (captured from serial), loop only. */
    real_bi = make_map(0x100000ULL, 0x1FEE0000ULL);
    real_end = 0x100000ULL + 0x1FEE0000ULL;
    start_up = vmm_rounded_start(real_bi.mmap[0].base);
    CHECK(start_up == 0x200000ULL, "real map also rounds up from 1 MiB to 2 MiB");
    CHECK(!vmm_loop_maps_frame(start_up, real_end, 0x100000ULL),
          "real region head is NOT covered by the loop");
    CHECK(PAGE_2MB(0x100000ULL) == 0,
          "real region head relies on vmm.c:60's unconditional first 2 MiB");
    CHECK(vmm_loop_maps_frame(start_up, real_end, 0x300000ULL),
          "real region interior is covered by the loop");
    CHECK(vmm_loop_maps_frame(start_up, real_end, 0x1FFD0000ULL),
          "real region tail is covered by the loop");

    /* Candidate A: clipping only the head, never the mapped tail. */
    CHECK(vmm_loop_maps_frame(0x200000ULL, 0x450000ULL, 0x440000ULL),
          "tail frame of [0x300000,0x450000) is already mapped");
    CHECK((0x450000ULL & ~(TWO_MB - 1)) == 0x400000ULL,
          "clipping the end down would exclude that mapped tail frame");
    CHECK(0x400000ULL >= 0x301000ULL,
          "a 1-page region at 3 MiB needs a lo < end guard before release");

    /* Separate, out-of-scope defect: kernel/vmm.c:51 drops physical bits >= 32. */
    CHECK((0x100000000ULL & VMM_PDE_ADDR_MASK) == 0,
          "2 MiB PDE mask aliases 4 GiB to physical zero");

    printf("identity-map gap proof: %d passed, %d failed\n", run, fail);
    return fail ? 1 : 0;
}
```

Build/run and result. **These paths are relative to the throwaway copy**, not to the
working tree: the copy root is
`C:\Users\wadia\AppData\Local\Temp\opencode\arc_mmap_probe`, and the commands below
were executed there with `cmd /c`. Running them in the repository would create
untracked files under `tests\`, which is why they are shown with that context.

```text
gcc -O2 -Wall -Wextra -std=gnu11 -I kernel \
    tests/test_identity_map_gap.c kernel/pmm.c -o tests/test_identity_map_gap.exe
tests\test_identity_map_gap.exe
identity-map gap proof: 15 passed, 0 failed
```

All fifteen checks pass: they assert the gap **exists**, that the real PMM allocator
really returns the uncovered frame, that the real map's head is covered only by the
unconditional first-page rule (not by the region loop), and that the adjacent
defects are real.

## 3. Verification against the real memory map (not a guess)

The actual multiboot map was captured by temporarily serial-printing `bi.mmap[]` in
the same throwaway copy, built and booted with the unmodified `-m 512M` QEMU gate
(`tools/boot_test.py`). Values below are decimal 64-bit; the first probe used
`terminal_print_hex()`, which prints only 32 bits and made the >4 GiB entry look
like zeros. The corrected capture is:

```text
BOOT: Multiboot1
MMAP_PROBE count=7
entry 0 base=0        len=654336       type=1  AVAILABLE [0, 0x9FC00)
entry 1 base=654336    len=1024         type=2  reserved
entry 2 base=983040    len=65536        type=2  reserved
entry 3 base=1048576   len=535691264    type=1  AVAILABLE [0x100000, 0x1FFE0000)
entry 4 base=536739840 len=131072       type=2  reserved
entry 5 base=4294705152 len=262144      type=2  reserved [0xFFFC0000, 0x100000000)
entry 6 base=1086626725888 len=12884901888 type=2 reserved [0xFD00000000, +0x300000000)
Physical memory: 511 MiB total, 500 MiB available
```

This is the normalized map that `pmm_init()`/`vmm_init()` consume: zero-length
entries are dropped by `kernel/bootinfo.c:59` and `:74`, and all seven printed
entries have nonzero length. Findings:

- The large AVAILABLE region starts at `0x100000`, which is **not** 2 MiB-aligned.
  `vmm_init()` rounds its start up to `0x200000`, so it does *not* map `[0, 2 MiB)`
  for that region. The current QEMU gate nevertheless passes because
  `vmm_map_2mb(0, 0)` already maps `[0, 2 MiB)` unconditionally — confirmed in §2
  by the check that the region head is absent from the loop.
- Therefore the shipped gate is green **by accident**, not because the rounding is
  correct. Any AVAILABLE region whose unaligned base lies at or above 2 MiB (for
  example the synthetic `[0x300000, 0x400000)` above) has no such safety net.
- End handling is already correct: the loop maps at 2 MiB-aligned `a` while
  `a < claimed_end`, so the last mapped page covers the partial tail. With that
  loop shape, rounding the end *up* changes nothing for coverage; it is not the
  fix. The defect is solely the rounded-up **start**.
- Entry 6 is reserved and starts above 4 GiB, so PMM does not allocate it today;
  it still demonstrates that firmware maps in this environment contain high
  physical addresses, which is why the separate `kernel/vmm.c:51` mask defect
  cannot be dismissed as theoretical.

## 4. Candidate fixes and tradeoffs

### A. Constrain PMM to frames VMM has proven mapped

Clip each released AVAILABLE range at init to a whole-page start:
`lo = round_up_2MiB(base)`, `end = base + len`, and release `[lo, end)` **only if
`lo < end`**. Do **not** also round the end down: the existing loop already maps the
page containing the tail, so clipping the end would discard usable frames for no
safety benefit (demonstrated in §2). The `lo < end` guard is mandatory: for a small
region such as `[0x300000, 0x301000)`, `lo = 0x400000 > end`; computing `end - lo`
in `uint64_t` wraps, and `pmm_release()` would clamp to `PMM_MAX_PHYS` and free
nearly the whole 16 GiB window — the opposite of the intended safety property.

The alternative is to keep the raw ranges but skip unaligned frames in
`pmm_alloc_pages()` using a mapped-range query.

- Pros: PMM can never return an unmapped frame; no change to `vmm_init()`; the
  invariant becomes explicit and locally enforceable.
- Cons: loses the head partial page of each region (up to 2 MiB), and a region
  smaller than one 2 MiB page with an unaligned base loses **all** its frames;
  requires the underflow guard above; `pmm_region_get()` would either report a
  range different from the firmware map or need a second "allocatable" meaning,
  which may affect existing accounting and host tests. A check inside
  `pmm_alloc_pages()` is feasible — `vmm_init()` does not allocate from PMM (§1) —
  but it couples PMM to VMM state: it must use a precomputed mapped-range set
  rather than a live page-table walk before CR3 is loaded, and it depends on
  `pmm_init`/`vmm_init` boot ordering staying as it is.

### B. Round the VMM identity-map start down (one-line change in `vmm_init()`)

Replace `start = (r.base + 0x1FFFFF) & ~0x1FFFFFULL` with
`start = r.base & ~0x1FFFFFULL`. Keep the loop and its `a < claimed_end` condition.

- Pros: restores the **PMM/VMM allocation contract for the range exercised today** —
  every frame PMM can allocate from a recorded AVAILABLE region below 4 GiB has an
  identity-map entry; verified in §2 against both the synthetic unaligned case and
  the real QEMU map. It does not discard any PMM frames; PMM, `pmm_region_get()`,
  and the syscall/ABI surface stay untouched.
- Cons: maps up to one extra 2 MiB page below a region start, which may include a
  reserved hole; harmless because no PMM frame is allocated from that hole, but it
  widens the identity map slightly. With `r.base = 0x100000` it duplicates the
  unconditional `[0, 2 MiB)` entry, which is an idempotent PDE write. It touches a
  frozen file (`kernel/vmm.c`), so it must not be done without approval. It is
  **necessary but not sufficient** for full 16 GiB correctness: it does not add
  reserved regions to `pmm_regions[]` and does not fix the >4 GiB address-mask
  defect. Both remain out of scope for this decision.

### C. Runtime check at each identity-map dereference

Add a check (panic/safe error) wherever a PMM-returned PA is dereferenced, e.g. via
a `vmm_identity_covers(pa)` helper.

- Pros: fails loudly instead of silently faulting; defense in depth if the range
  contract is later broken by a different firmware map; useful diagnostics.
- Cons: the dereference sites are numerous and scattered — `slab.c`, `memory.c`,
  `fs.c`, `gfx.c`, `gui.c`, `exec.c`, `address_space.c`, `scheduler.c`, `ahci.c` all
  allocate and then write PMM pages. Centralizing in `pmm_alloc_pages()` is
  possible in principle (§1) but introduces PMM→VMM coupling and a pre-init window
  that must be handled explicitly. This is many edits with many failure paths and
  no single place that guarantees the invariant; it detects the bug rather than
  preventing it and expands the frozen scope well beyond this slice.

## 5. Recommendation

Choose **B** as the primary fix: round the region start **down** in `vmm_init()` and
leave the loop/end logic alone. It is a one-line correction that removes the
allocatable-but-unmapped frame gap for the range this kernel exercises today, and it
is backed by both the real QEMU map and the synthetic host proof above. It discards
no PMM frames and does not touch PMM accounting, syscall routing, or the frozen
`tests\run_all.bat`.

This is a **necessary, not sufficient**, step toward full-range correctness: the
separate `kernel/vmm.c:51` address-mask defect still breaks identity mapping for
physical pages at or above 4 GiB even after B. Approval of B should not be read as
approval to claim full 16 GiB coverage; that needs its own design note and fix.

Do **not** choose A as the primary fix: discarding usable RAM to compensate for a
VMM rounding bug is a permanent capacity cost for a transient map-edge problem,
and its naive form can free almost the entire PMM window through unsigned wrap.
Treat C as a possible follow-up hardening item, not the fix: it needs many call-site
changes and cannot enforce the invariant centrally without new coupling.

Suggested follow-up work **after** approval of B (not done here): promote the host
proof into a permanent regression test that asserts, for arbitrary region bases,
that `PAGE_2MB(pa)` is in the identity-map set for every frame PMM can allocate;
then run the user-access suite, the full build, `tests\run_all.bat`, and the QEMU
pointer gate. The >4 GiB mask fix is a separate decision.

## 6. Explicitly not done

- No fix implemented; `kernel/pmm.c`, `kernel/vmm.c`, and all tests in the repository
  are untouched. Items 1, 2, and 4 of the earlier review list remain untouched;
  review item 1's committed fix (`c025f45`) is unchanged.
- The proof test and the serial-print probe lived only in a throwaway directory
  under `C:\Users\wadia\AppData\Local\Temp\opencode\arc_mmap_probe`; nothing from it
  was staged or committed.
- No commit, push, merge, or tag was made. Cross-model review was skipped at the
  user's direction; only the single-model adversarial review was used.

## 7. Review fixes applied (first review cycle)

| # | Finding from adversarial review | Where fixed in this note |
|---|---|---|
| 1 | Claim that `vmm_init()` allocates page tables from PMM was false; it does not, so a centralized check is not ruled out by circular dependency. | §1 boot-order facts; §4 A and C rewritten to state the real PMM→VMM coupling instead. |
| 2 | "Restores contract to map all physical regions" overstated: `pmm_regions[]` holds only AVAILABLE entries, and the reserved-block coverage was incidental. | §1 related facts; §5 recommendation narrowed to the PMM/VMM allocation contract. The reviewer's stronger sub-claim that the reserved block is "never mapped at all" was rejected: the AVAILABLE region's tail page `0x1FE00000` spans it. |
| 3 | `kernel/vmm.c:51` uses a 32-bit mask, aliasing PAs >= 4 GiB; PMM can allocate to 16 GiB. | §1 separate defect; §2 test check; §3 map evidence; §4 B cons; §5 explicit out-of-scope sequence. |
| 4 | Candidate A's end-clipping would discard already-mapped tail frames, overstating its cost. | §4 A now clips only the head; §2 test demonstrates the tail loss. |
| 5 | Real-map control was tautological (`page_base == 0` returned success unconditionally). | §2 test now models the region loop separately, asserts the head is *absent* from it, and identifies the first-page rule explicitly. |
| 6 | Build command appeared to write into `tests\` while claiming no repository change. | §2 now states explicitly that paths are relative to the throwaway copy and gives its absolute root. |

## 8. Review fixes applied (second review cycle)

| # | Finding from adversarial review | Where fixed in this note |
|---|---|---|
| 7 | Candidate A's `lo = round_up(base)` recipe underflows for a region smaller than one 2 MiB page, potentially freeing nearly the whole PMM window. | §4 A now requires a `lo < end` guard and names the failure mode; §2 test records the guard requirement. |
| 8 | The captured map evidence was misread because `terminal_print_hex()` prints only 32 bits, making the >4 GiB entry look zero-length. | §3 now shows the corrected 64-bit decimal capture, explains the earlier truncation, and notes all seven entries are nonzero; §1 cites the high reserved entry. |
| 9 | The "head covered" control was still tautological because the helper returned success for `page == 0`. | §2 helper no longer has that shortcut; it models only the loop, and separate checks assert the head is absent from the loop and relies on `vmm.c:60`. |
| 10 | §5 still claimed the VMM would "honour the contract" without qualification despite the >4 GiB mask defect. | §5 rewritten as "necessary, not sufficient", scoped to <4 GiB and to "discards no PMM frames"; §4 B cons state the same limit. |
| 11 | The host-stub rationale wrongly claimed the symbols were outside PMM's 16 GiB window. | §2 now describes MinGW image-base placement and relies on assertions, not on linker address. |
| 12 | The modeled loop omitted the `VMM_PD_COUNT`/PML4 guard. | §2 helper now includes both guards and cites `kernel/vmm.c:16,50`; tested addresses are all below 512 MiB. |
