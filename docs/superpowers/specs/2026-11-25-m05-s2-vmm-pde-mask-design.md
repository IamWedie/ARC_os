# M0.5 §2 — `vmm.c:51` 2 MiB PDE address-mask fix

Status: **approved task; design note written before code, as required by the
operating contract.** This note covers only the PDE mask. No Phase A/B/C
syscall-routing work is included.

Baseline at the time of writing: branch `feature/m05-s2-user-pointer-first-slice`,
HEAD `660ac89ef09e9330465d22a9f4fdb5f085bf79d5`. The tree is not pristine:
`tasks/todo.md` has the previously approved HIGH PRIORITY risk-statement edit
and the item-5 syscall-pointer design note is untracked. Neither touches
`kernel/vmm.c`, `kernel/pmm.c`, `kernel/pagetable.c`, `kernel/user_access.c`, or
any existing test, so the RED run can be attributed to `vmm.c:51`.

## 1. Root cause (confirmed against current source)

`kernel/vmm.c:45-52` installs the kernel identity map's 2 MiB pages:

```c
vmm_pd[pdpti][pdi] = (paddr & 0xFFE00000ULL) | PG_PRESENT | PG_RW | PG_PSE;
```

`0xFFE00000ULL` has bits 21–31 set and **no bits ≥ 32**. A 2 MiB-aligned
physical address at or above 4 GiB is therefore truncated to its low 32 bits
and the PDE points at a different physical page. The correct 2 MiB address
mask on a 64-bit entry is `0x000FFFFFFFE00000ULL`: the same 52-bit physical
address field the rest of the paging core uses (`kernel/pagetable.c:12`,
`kernel/user_access.c:12`) with the low 21 bits cleared.

The defect is reachable, not theoretical:

- `kernel/pmm.c:13` permits allocation up to 16 GiB; `kernel/pmm.c:132-151`
  returns the first free frame, so a firmware map with AVAILABLE memory above
  4 GiB yields such frames.
- `kernel/vmm.c:16,50` maps up to 128 GiB (`VMM_PD_COUNT = 128`), so a PA in
  `[4 GiB, 128 GiB)` passes the `pml4i`/`pdpti` guard and is actually mapped —
  to the wrong page.
- The captured real QEMU memory map (`docs/.../2026-11-24-m05-s2-identity-map-gap-design.md:211`)
  contains entry 6, base `0xFD00000000` (1086626725888), length `0x300000000`,
  type reserved. That entry itself is above the 128 GiB map limit and reserved,
  so it is **not** an allocation candidate; it is evidence that firmware in
  this environment really reports ≥4 GiB physical addresses.

## 2. Why this is in the syscall trust chain

`kernel/user_access.c:123-124` and `:153-154` derive a physical address from a
validated user leaf (`pa = (*leaf & UA_ADDR_MASK) + offset`) and dereference it
directly with `memcpy`. That is correct only if the kernel slot-0 identity map
translates `pa` to itself. With the bad PDE mask, a user page whose backing
frame is ≥4 GiB makes even a fully validated `SYS_READ`/`SYS_WRITE` access an
unrelated physical page — potentially kernel memory. The same identity map is
also used by `ua_walk()` at `user_access.c:44-57` to dereference the PML4,
PDPT, PD and PT tables; high physical table frames are aliased too. Validating
the user VA does not help; the address translation below it is wrong.

## 3. Fix options

| Option | Change | Verdict |
|---|---|---|
| A. Change only the mask literal at `vmm.c:51` to `0x000FFFFFFFE00000ULL` | One token; preserves all physical bits and keeps bits 63:52 clear like the rest of the paging core. | **Chosen** — smallest correct fix. |
| B. Use `~0x1FFFFFULL` | Also one token, but would propagate any bogus bits ≥52 from a malformed input into the PDE, where they are architecturally reserved. | Rejected. |
| C. Add a runtime range check / panic at every identity-map dereference | Many call sites; detects rather than prevents; broad frozen-file churn. | Rejected (out of scope). |
| D. Extend `VMM_PD_COUNT` to cover the >128 GiB firmware region | Separate capacity/range issue; does not correct the mask. | Rejected (out of scope). |

No syscall number, ABI, PMM, `user_access.c`, or existing gate changes.

## 4. Proof plan (RED then GREEN)

New host test `tests/test_vmm_pde_mask.c`, wired into `tests/run_tests.bat`:

- Compiles the **real** `kernel/vmm.c` into the test translation unit via
  `#include "../kernel/vmm.c"`, so the test calls the production static
  `vmm_map_2mb()` and reads the production `vmm_pd[]` entry it writes. Only
  that function is called; the functions containing privileged `invlpg`/CR3
  access (`vmm.c:25-27,83,86-93`) are never executed. An adversarial review
  showed the earlier source-text-matching version could pass while the built
  kernel used different code; this version has no source-literal model.
- Does **not** scan or link `kernel/user_access.c`. Phase A/B/C will
  legitimately edit that file; gating this PDE-mask regression on its current
  text would couple two unrelated workstreams. The `copy_*_user()` PA
  derivation and `ua_walk()` table-PA extraction are modeled in the test with
  the consumer-side 52-bit `UA_ADDR_MASK` equivalent and source citations.
- Links the **real** `kernel/pmm.c` and `kernel/pagetable.c` (with
  `tests/pt_stub.c`). All page-table allocation/validation decisions are made
  by production code; only the host's inability to dereference ≥4 GiB physical
  addresses is modeled by comparing computed translations instead of doing
  the final `memcpy`.
- Defines `__kernel_start`/`__kernel_end` host stubs and models
  `pmm_reserve()` exactly (`kernel/pmm.c:64-70`): a base at/above 16 GiB is
  ignored; a reversed range wraps, is clamped to 16 GiB, and reserves
  `[base, 16 GiB)`. The test checks that the resulting reservation cannot
  overlap any synthetic high-PA region in either link order. This prevents an
  unrelated PMM failure from being mistaken for a mask failure.
- Asks the real PMM for a frame in a synthetic AVAILABLE region starting at
  exactly 4 GiB (and separately 8 GiB and 12 GiB, all below `PMM_MAX_PHYS`),
  and verifies each frame passes `vmm_map_2mb()`'s PML4/PDPT guard.
- Links the **real** `kernel/pagetable.c` with `tests/pt_stub.c`, maps a user VA
  to that frame with `pt_map()`, reads the leaf with `pt_lookup()`, and models
  the exact `user_access.c` PA derivation plus the identity-map PDE lookup.
- Asserts that `paddr`, the PDE address produced by the production function,
  and the final dereferenced physical address are all the same 2 MiB page,
  for 4 GiB, 8 GiB and 12 GiB (real frames from the linked PMM; all below
  `PMM_MAX_PHYS = 16 GiB`) and 64 GiB (numeric case inside the 128 GiB map
  range, above PMM's ceiling).
- Mirrors `ua_walk()`'s four table-PA extractions (`user_access.c:44-57`) with
  synthetic high entries and requires each table PA to identity-translate to
  itself through the production PDE. Low-PA controls (`0`, `0xFFE00000`) are
  checked the same way.
- Uses direct mask checks (calling production `vmm_map_2mb()` with `vaddr=0`)
  for cases outside the identity-map call pattern: the captured firmware high
  PA `0xFD00000000` (reserved and beyond `VMM_PD_COUNT`), a synthetic address
  with bit 51 set plus reserved bits ≥52, and a malformed address with bits
  0-20 set. These fail a mask that drops bit 51, a mask that propagates bits
  ≥52, and a mask that fails to clear the low 21 bits respectively. It also
  checks that the production write always sets PRESENT|RW|PSE.

Expected: **RED** with the original mask (the production PDE aliases 4 GiB to
physical page 0), then **GREEN** after the one-token fix. The strengthened
test's RED run is captured by temporarily restoring the original mask, with
no other source change, before the final GREEN run.

## 5. Verification and limits

- Focused: build and run the new test before and after the fix, with exact
  commands and exit codes.
- Boundary: full matrix — `tests\run_tests.bat`, `python tools\build_kernel.py
  --quiet`, `tests\run_all.bat`, `tests\boot_test_syscall_ptr.bat`, and
  `.\tests\test_identity_map.exe`.
- Host tests model the identity map; they do not boot a kernel with ≥4 GiB RAM.
  No QEMU or hardware claim is made for the high-PA path itself. QEMU gates are
  run only as regression checks for unchanged behavior.
- The test exercises behavior, not literal spelling: an equivalent correct
  expression or named constant would still pass. This is intentional after
  review — the regression guards the translation result, not the source text.
- The ≥4 GiB addresses exercised through the real identity-map call pattern
  are concrete frames returned by the linked PMM from synthetic AVAILABLE
  regions; they are not a captured firmware mapping. The one captured firmware
  high address (`0xFD00000000`) is reserved and outside the frozen 128 GiB map
  range, so it can only be used for direct mask arithmetic, not for a real
  identity-map call. Claiming otherwise would require changing `VMM_PD_COUNT`,
  which is out of scope.
- `tests/run_tests.bat` gains one additional case as `[21/21]`; existing step
  labels are left untouched to keep the diff small despite their pre-existing
  inconsistency. The full matrix is run in the order given by the approver:
  `run_tests.bat`, `build_kernel.py --quiet`, `run_all.bat`,
  `boot_test_syscall_ptr.bat`, `test_identity_map.exe`.
- Out of scope and still open: reserved-region mapping in `vmm_init()`,
  `VMM_PD_COUNT`'s 128 GiB ceiling, and the Phase A/B/C syscall-pointer routing
  work.
