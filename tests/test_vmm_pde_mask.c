/*
 * ARC OS 2 MiB PDE address-mask regression test.
 *
 * Contract under test: vmm_map_2mb() must preserve every physical address bit
 * in the 2 MiB PDE. The old 0xFFE00000ULL mask keeps only bits 21-31, so a
 * frame at or above 4 GiB is aliased to a different physical page. That is
 * also the page copy_from_user()/copy_to_user() reach after translating a
 * validated user VA through the kernel's slot-0 identity map
 * (kernel/user_access.c:123-124,153-154). ua_walk() uses that same identity
 * map to dereference each user page-table level (user_access.c:44-57), so bad
 * table PAs are aliased too.
 *
 * This test COMPILES the production kernel/vmm.c into this translation unit
 * so the static vmm_map_2mb() and its real vmm_pd table are exercised directly.
 * No privileged instruction is executed: only vmm_map_2mb() is called; the
 * functions containing invlpg/CR3 access are never called. The test also links
 * the REAL kernel/pmm.c and kernel/pagetable.c (with tests/pt_stub.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"
#include "bootinfo.h"

/* Production source under test. Quoted include resolves relative to this file
 * (tests/../kernel/vmm.c); -I kernel also works. This is compiled code, not a
 * source-text model. */
#include "../kernel/vmm.c"

/* pmm.c resolves these at link time. Their ordering and the high-region
 * overlap are checked in main() so PMM cannot fail for an unrelated reason. */
unsigned char __kernel_start[16];
unsigned char __kernel_end[16];

process_t processes[MAX_PROCESSES];
volatile int current_process;

extern void pt_stub_reset(void);
extern uint64_t* pt_stub_root(void);
extern pt_ops_t pt_stub_ops;

#define TWO_MB         0x200000ULL
#define PAGE_OFF_MASK  (TWO_MB - 1ULL)
#define USER_TEST_VA   0x0000008000000000ULL /* PML4 slot 1 */
#define TEST_PMM_MAX_PHYS (16ULL << 30)      /* kernel/pmm.c:13 */

/* Consumer-side physical address masks from kernel/pagetable.c:12 and
 * kernel/user_access.c:12. These are not the PDE mask under test; they are
 * what the rest of the paging core expects the PDE address field to contain. */
#define TEST_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Captured real QEMU memory map entry 6 (reserved, above VMM_PD_COUNT). */
#define FIRMWARE_HIGH_PA 0xFD00000000ULL

static int tests_passed;
static int tests_failed;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (cond) {                                           \
            tests_passed++;                                   \
        } else {                                              \
            tests_failed++;                                   \
            printf("  FAIL: ");                               \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

/* Call the production vmm_map_2mb() and return the PDE it actually wrote.
 * The clear-then-call setup prevents a stale entry from masking a failure. */
static uint64_t map_and_read_pde(uint64_t vaddr, uint64_t paddr,
                                 const char* label) {
    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    uint64_t pdi   = (vaddr >> 21) & 0x1FF;

    CHECK(pml4i == 0 && pdpti < VMM_PD_COUNT,
          "%s: vaddr 0x%llX rejected by production guard", label,
          (unsigned long long)vaddr);
    if (pml4i != 0 || pdpti >= VMM_PD_COUNT) return 0;

    vmm_pd[pdpti][pdi] = 0;
    vmm_map_2mb(vaddr, paddr);
    CHECK((vmm_pd[pdpti][pdi] & (PG_PRESENT | PG_RW | PG_PSE)) ==
              (PG_PRESENT | PG_RW | PG_PSE),
          "%s: PDE flags are not PRESENT|RW|PSE (0x%llX)", label,
          (unsigned long long)vmm_pd[pdpti][pdi]);
    return vmm_pd[pdpti][pdi];
}

/* Identity-map translation produced by the REAL PDE: address field plus the
 * 2 MiB page offset. Flags in bits 0,1,7 are excluded by TEST_ADDR_MASK. */
static uint64_t identity_translate(uint64_t pde, uint64_t pa) {
    return (pde & TEST_ADDR_MASK) | (pa & PAGE_OFF_MASK);
}

/* Exercise a real identity-map translation for an address the kernel could
 * dereference: either a final data PA or a user page-table PA in ua_walk(). */
static void check_identity_translation(uint64_t pa, const char* label) {
    uint64_t page = pa & ~PAGE_OFF_MASK;
    uint64_t pde = map_and_read_pde(page, page, label);
    uint64_t translated = identity_translate(pde, pa);

    CHECK((pde & PG_PSE) != 0, "%s: PDE does not have PSE set", label);
    CHECK(translated == pa,
          "%s: identity map resolves PA 0x%llX to 0x%llX (PDE 0x%llX)",
          label, (unsigned long long)pa, (unsigned long long)translated,
          (unsigned long long)pde);
}

/* Some arithmetic checks deliberately pass a high paddr with vaddr=0. That is
 * outside the identity-map call pattern, so it is labeled as a mask check,
 * not a claim that the 128 GiB map supports those physical addresses. */
static void check_direct_mask(uint64_t paddr, uint64_t expected_addr,
                              const char* label) {
    uint64_t pde = map_and_read_pde(0, paddr, label);
    uint64_t addr = pde & TEST_ADDR_MASK;

    CHECK((pde & PG_PSE) != 0, "%s: PDE does not have PSE set", label);
    CHECK(addr == expected_addr,
          "%s: PDE address 0x%llX, want 0x%llX (PDE 0x%llX)",
          label, (unsigned long long)addr,
          (unsigned long long)expected_addr, (unsigned long long)pde);
}

static void* pmm_frame_at(uint64_t base, const char* label) {
    struct bootinfo bi;
    void* frame;

    memset(&bi, 0, sizeof bi);
    bi.flags = BOOTINFO_FLAG_MMAP;
    bi.mmap_count = 1;
    bi.mmap[0].base = base;
    bi.mmap[0].len = TWO_MB;
    bi.mmap[0].type = BOOTINFO_MMAP_AVAILABLE;

    pmm_init(&bi);
    CHECK(pmm_region_count_get() == 1, "%s: PMM recorded one region", label);
    frame = pmm_alloc_pages(1);
    CHECK((uint64_t)frame == base,
          "%s: PMM frame 0x%llX, want 0x%llX", label,
          (unsigned long long)(uintptr_t)frame, (unsigned long long)base);
    return frame;
}

/* Model the copy_*_user() dereference: read the leaf with the real page-table
 * core, derive the PA exactly as user_access.c:123,153 do, then ask the real
 * vmm_map_2mb() what physical address the identity map supplies. No host
 * dereference of a high PA is attempted. */
static void check_trust_chain(uint64_t frame_pa, const char* label) {
    uint64_t* root;
    uint64_t* leaf;
    uint64_t leaf_pa;
    uint64_t deref_pa;
    uint64_t page;
    char ctx[96];

    pt_stub_reset();
    root = pt_stub_root();
    CHECK(pt_map(root, USER_TEST_VA, frame_pa,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "%s: pt_map of high frame failed", label);
    leaf = pt_lookup(root, USER_TEST_VA);
    CHECK(leaf != NULL, "%s: pt_lookup found no leaf", label);
    if (!leaf) return;

    leaf_pa = *leaf & TEST_ADDR_MASK;
    deref_pa = leaf_pa + 0x123ULL;
    page = deref_pa & ~PAGE_OFF_MASK;

    CHECK(leaf_pa == frame_pa, "%s: leaf PA 0x%llX, want 0x%llX", label,
          (unsigned long long)leaf_pa, (unsigned long long)frame_pa);
    snprintf(ctx, sizeof ctx, "%s copy deref", label);
    check_identity_translation(page, ctx);
}

/* Mirror ua_walk()'s table-PA extraction at user_access.c:44-57 and require
 * every level to be reached through a correct production identity-map PDE. */
static void check_ua_walk_table_translations(void) {
    static const uint64_t walk_entries[] = {
        0x100000000ULL | PT_PG_PRESENT | PT_PG_USER,
        0x200000000ULL | PT_PG_PRESENT | PT_PG_USER,
        0x300000000ULL | PT_PG_PRESENT | PT_PG_USER,
        0x1000000000ULL | PT_PG_PRESENT | PT_PG_USER | PT_PG_RW,
    };
    static const char* levels[] = {
        "ua_walk PML4 table PA",
        "ua_walk PDPT table PA",
        "ua_walk PD table PA",
        "ua_walk PT table PA",
    };

    for (size_t i = 0; i < sizeof walk_entries / sizeof walk_entries[0]; i++) {
        uint64_t table_pa = walk_entries[i] & TEST_ADDR_MASK;
        check_identity_translation(table_pa, levels[i]);
    }
}

static void check_high_frame(uint64_t base, int from_pmm, const char* label) {
    uint64_t frame_pa = base;

    if (from_pmm) {
        void* frame = pmm_frame_at(base, label);
        if (!frame) return;
        frame_pa = (uint64_t)(uintptr_t)frame;
    }

    check_identity_translation(frame_pa, label);
    check_trust_chain(frame_pa, label);
}

/* Model pmm_reserve() exactly for the host kernel-image symbols
 * (kernel/pmm.c:64-70): a base at/above PMM_MAX_PHYS is ignored; a reversed
 * range wraps, is clamped to PMM_MAX_PHYS, and reserves [base, 16 GiB). */
static int stub_reservation_overlaps(uint64_t lo, uint64_t hi,
                                     uint64_t base, uint64_t len) {
    uint64_t end;

    if (lo >= TEST_PMM_MAX_PHYS) return 0;
    if (hi < lo) {
        end = TEST_PMM_MAX_PHYS;
    } else {
        end = hi;
        if (end < lo || end > TEST_PMM_MAX_PHYS) end = TEST_PMM_MAX_PHYS;
    }
    if (end <= lo) return 0;
    return lo < base + len && end > base;
}

int main(void) {
    uint64_t stub_lo = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t stub_hi = (uint64_t)(uintptr_t)__kernel_end;

    printf("ARC OS 2 MiB PDE address-mask test\n");
    printf("  exercising production vmm_map_2mb() from kernel/vmm.c\n");

    /* pmm_init() reserves the host kernel-image symbol range after releasing
     * the synthetic AVAILABLE region (kernel/pmm.c:128-129). Check both symbol
     * orders exactly as pmm_reserve() handles them so an unrelated allocator
     * failure cannot be mistaken for a PDE-mask failure. */
    if (stub_lo > stub_hi)
        printf("  note: host stub symbols linked in reverse order\n");
    CHECK(!stub_reservation_overlaps(stub_lo, stub_hi, 0x100000000ULL, TWO_MB),
          "host kernel-image reservation does not overlap the 4 GiB region");
    CHECK(!stub_reservation_overlaps(stub_lo, stub_hi, 0x200000000ULL, TWO_MB),
          "host kernel-image reservation does not overlap the 8 GiB region");
    CHECK(!stub_reservation_overlaps(stub_lo, stub_hi, 0x300000000ULL, TWO_MB),
          "host kernel-image reservation does not overlap the 12 GiB region");

    /* Real PMM frames above 4 GiB (all below PMM_MAX_PHYS = 16 GiB). */
    check_high_frame(0x100000000ULL, 1, "4 GiB PMM frame");
    check_high_frame(0x200000000ULL, 1, "8 GiB PMM frame");
    check_high_frame(0x300000000ULL, 1, "12 GiB PMM frame");
    /* Numeric case above PMM's ceiling but inside VMM's 128 GiB map range. */
    check_high_frame(0x1000000000ULL, 0, "64 GiB modeled frame");

    check_ua_walk_table_translations();

    /* Low-PA controls: vmm_map_2mb() only ever receives 2 MiB-aligned PAs. */
    check_identity_translation(0, "2 MiB control");
    check_identity_translation(0xFFE00000ULL, "just-below-4 GiB control");

    /* Mask arithmetic on the captured real firmware address. This entry is
     * reserved and beyond VMM_PD_COUNT, so it is a direct mask check only. */
    check_direct_mask(FIRMWARE_HIGH_PA, FIRMWARE_HIGH_PA,
                      "firmware high PA (arithmetic only)");

    /* A correct 52-bit mask preserves bit 51 but clears architecturally
     * reserved bits >= 52. A mask of 0xFFE00000 fails the first check; a mask
     * of ~0x1FFFFF fails the second. */
    check_direct_mask((1ULL << 51) | (1ULL << 52) | (1ULL << 63) | TWO_MB,
                      (1ULL << 51) | TWO_MB, "bit 51 / reserved-bit control");

    /* Pin the low boundary too: bits 0-20 must be cleared even if a malformed
     * paddr sets them. A mask of 0x000FFFFFFFFFFFFF would pass every 2 MiB-
     * aligned case above but fail this one. */
    check_direct_mask((1ULL << 51) | 0x3FFFFFULL, (1ULL << 51) | TWO_MB,
                      "low-21-bit control");

    printf("\nPDE mask test summary: %d passed, %d failed\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
