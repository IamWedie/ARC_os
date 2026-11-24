# M0.5 §2 bit-7 interpretation — PSE vs PAT

## Context

`ua_entry_ok()` in `kernel/user_access.c` validates PML4E, PDPTE, PDE, and the
final 4 KiB PTE. It currently applies the same bit-7 test at every level.

## x86-64 bit-7 meanings

- PML4E / PDPTE / PDE: bit 7 is PS when the CPU and the corresponding large or
  huge page size are supported (PML4E only with 512 GiB support; in this
  kernel's four-level model PDPTE = 1 GiB and PDE = 2 MiB are the applicable
  large-page levels). A set PS bit means the entry maps a large page and does
  not point to the next table.
- Final PTE for a 4 KiB page: bit 7 is PAT, a cache-attribute selector. It does
  not change the mapping size or the present/user/RW permissions.

## Current incorrect behavior

`ua_entry_ok()` runs `if (entry & PT_PG_PSE) return -1;` at all four levels. A
normal 4 KiB user page whose PTE has PAT set is therefore rejected as though it
were a large-page mapping. Concretely: map a present, user, writable 4 KiB page
with `pt_map()`, then set bit 7 on only its leaf entry. The mapping is still a
valid 4 KiB page, but `copy_from_user()` returns `-1` and copies no bytes.

## Corrected per-level interpretation

Pass a final-level flag to `ua_entry_ok()`:

- PML4E, PDPTE, PDE (`is_pte == 0`): bit 7 is PS/large-page. Keep rejecting it;
  the user-copy boundary needs a 4 KiB leaf, so the existing large-page policy
  is unchanged.
- Final PTE (`is_pte == 1`): ignore bit 7 as PAT. Keep the existing present,
  user, and write-RW checks.

No page-table API, kernel flag definition, syscall, ABI, or PMM change is
needed. Host tests set bits directly in real page-table entries through the
existing `tests/pt_stub.c` page source.
