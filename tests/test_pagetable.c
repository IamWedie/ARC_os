/*
 * ARC OS Page-Table Core Test
 *
 * Links against the REAL kernel/pagetable.c with tests/pt_stub.c providing
 * the page arena. The core performs no privileged instructions, so the whole
 * PML4 -> PDPT -> PD -> PT walk is exercised on the host.
 */
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Host page source (tests/pt_stub.c). */
extern void pt_stub_reset(void);
extern uint64_t* pt_stub_root(void);
extern void pt_stub_fail_after(int n);
extern int pt_stub_alloc_count(void);
extern int pt_stub_freed_count(void);
extern pt_ops_t pt_stub_ops;

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

/* Table entries point at 4 KiB pages; the low bits carry the flags. */
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

static void test_map_and_lookup(void) {
    printf("\n=== Test 1: map and lookup ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();             /* zeroed 4096-byte page */
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0, "map failed");
    uint64_t* leaf = pt_lookup(root, USER_CODE_VA);
    CHECK(leaf != NULL, "leaf missing");
    if (!leaf) return;
    CHECK(*leaf == (0x1000000 | PT_PG_PRESENT | PT_PG_USER | PT_PG_RW),
          "leaf wrong: 0x%llx", (unsigned long long)*leaf);
    CHECK(pt_count_user_pages(root) == 1, "page count != 1");
}

static void test_intermediate_reuse(void) {
    printf("\n=== Test 2: intermediate tables are reused ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    pt_map(root, USER_CODE_VA,      0x1000000, PT_PG_USER | PT_PG_RW, &pt_stub_ops);
    pt_map(root, USER_CODE_VA+4096, 0x1001000, PT_PG_USER | PT_PG_RW, &pt_stub_ops);
    CHECK(pt_stub_alloc_count() == 3, "expected 3 tables, got %d",
          pt_stub_alloc_count());               /* PDPT + PD + PT */
    CHECK(pt_count_user_pages(root) == 2, "page count != 2");
}

static void test_kernel_slot_not_counted(void) {
    printf("\n=== Test 3: shared kernel slot is not a user page ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    CHECK(pt_map(root, 0x200000, 0x200000, PT_PG_RW, &pt_stub_ops) == 0,
          "kernel map failed");
    CHECK(pt_lookup(root, 0x200000) != NULL, "kernel leaf missing");
    CHECK(pt_count_user_pages(root) == 0, "kernel page counted as user");
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0, "user map failed");
    CHECK(pt_count_user_pages(root) == 1, "user page count != 1");
}

static void test_unmap(void) {
    printf("\n=== Test 4: unmap clears the leaf ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0, "map failed");

    uint64_t* leaf = pt_lookup(root, USER_CODE_VA);
    CHECK(leaf != NULL, "leaf missing");
    CHECK(pt_unmap(root, USER_CODE_VA) == 0, "unmap failed");
    CHECK(leaf != NULL && *leaf == 0, "leaf not cleared");
    CHECK(pt_lookup(root, USER_CODE_VA) == NULL, "lookup after unmap");
    CHECK(pt_count_user_pages(root) == 0, "page count != 0");
    CHECK(pt_unmap(root, USER_CODE_VA) == -1, "unmapping twice accepted");
    CHECK(pt_unmap(root, USER_STACK_VA) == -1,
          "unmapping an absent mapping accepted");
}

static void test_large_page_refused(void) {
    printf("\n=== Test 5: existing large page is refused ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0, "setup map failed");

    /* Turn the PDPT entry covering USER_CODE_VA into a 1 GiB page. */
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(root[USER_CODE_VA >> 39] & PT_ADDR_MASK);
    uint64_t* pdpte = &pdpt[(USER_CODE_VA >> 30) & 0x1FF];
    uint64_t before = 0x40000000ULL | PT_PG_PRESENT | PT_PG_RW | PT_PG_PSE;
    *pdpte = before;

    CHECK(pt_map(root, USER_CODE_VA, 0x2000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == -1,
          "large page not refused");
    CHECK(*pdpte == before, "large-page entry changed: 0x%llx",
          (unsigned long long)*pdpte);
    CHECK(pt_lookup(root, USER_CODE_VA) == NULL, "lookup walked a large page");
}

static void test_alloc_failure(void) {
    printf("\n=== Test 6: allocation failure leaves a usable root ===\n");
    pt_stub_reset();
    uint64_t* root = pt_stub_root();
    pt_stub_fail_after(2);                       /* third allocation fails */
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000, PT_PG_USER, &pt_stub_ops) == -1,
          "alloc failure not reported");
    pt_stub_fail_after(-1);
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000, PT_PG_USER, &pt_stub_ops) == 0,
          "root unusable after partial failure");
    CHECK(pt_count_user_pages(root) == 1, "page count != 1");
}

int main(void) {
    printf("ARC OS page-table core test (real kernel/pagetable.c)\n");

    test_map_and_lookup();
    test_intermediate_reuse();
    test_kernel_slot_not_counted();
    test_unmap();
    test_large_page_refused();
    test_alloc_failure();

    printf("\n=== Results ===\n");
    printf("  passed: %d\n", tests_passed);
    printf("  failed: %d\n", tests_failed);
    if (tests_failed == 0) {
        printf("\n  ALL TESTS PASSED\n");
        return 0;
    }
    printf("\n  TESTS FAILED\n");
    return 1;
}
