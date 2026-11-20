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
extern void pt_stub_reset_freed_count(void);
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
    /* The failed attempt kept its two linked tables; the retry allocates only
     * the missing PT. 3 allocations means no page was orphaned by the failure. */
    CHECK(pt_stub_alloc_count() == 3, "retry did not reuse partial tables: %d",
          pt_stub_alloc_count());
    CHECK(pt_count_user_pages(root) == 1, "page count != 1");
}

static void test_clone_shared(void) {
    printf("\n=== Test 7: clone shares only the kernel slot ===\n");
    pt_stub_reset();
    uint64_t* master = pt_stub_root();
    uint64_t* child = (uint64_t*)pt_stub_ops.alloc();
    CHECK(child != NULL, "child root allocation failed");
    if (!child) return;

    /* master slot 0 = 0xABC000|PT_PG_PRESENT|PT_PG_RW; slot 1 = sentinel */
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;
    master[PT_USER_SLOT_FIRST] = 0xDEF000ULL | PT_PG_PRESENT | PT_PG_USER;
    master[2] = 0xFEE000ULL | PT_PG_PRESENT;

    pt_clone_shared(master, child);
    CHECK(child[0] == master[0], "kernel slot not shared");
    CHECK(child[PT_USER_SLOT_FIRST] == 0, "user slot copied");
    CHECK(child[2] == 0, "unused slot copied");
    CHECK(pt_stub_alloc_count() == 1, "clone allocated a table");
}

static void test_isolation(void) {
    printf("\n=== Test 8: cloned roots have private user leaves ===\n");
    pt_stub_reset();
    uint64_t* master = pt_stub_root();
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;

    /* two roots cloned from the same master; map different paddr at USER_CODE_VA */
    uint64_t* a = (uint64_t*)pt_stub_ops.alloc();
    uint64_t* b = (uint64_t*)pt_stub_ops.alloc();
    CHECK(a != NULL && b != NULL, "root allocation failed");
    if (!a || !b) return;

    pt_clone_shared(master, a);
    pt_clone_shared(master, b);
    CHECK(pt_map(a, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "map in root A failed");
    CHECK(pt_map(b, USER_CODE_VA, 0x2000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "map in root B failed");

    uint64_t* leaf_a = pt_lookup(a, USER_CODE_VA);
    uint64_t* leaf_b = pt_lookup(b, USER_CODE_VA);
    CHECK(leaf_a != NULL && leaf_b != NULL, "user leaf missing after map");
    if (!leaf_a || !leaf_b) return;
    CHECK(*pt_lookup(a, USER_CODE_VA) != *pt_lookup(b, USER_CODE_VA),
          "processes share a leaf");
    CHECK(a[0] == master[0] && b[0] == master[0], "kernel slot not shared");
    CHECK(a[PT_USER_SLOT_FIRST] != b[PT_USER_SLOT_FIRST],
          "private PDPT shared");
}

static void test_free_user_tables(void) {
    printf("\n=== Test 9: teardown frees exactly five private tables ===\n");
    pt_stub_reset();
    uint64_t* master = pt_stub_root();
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;

    /* USER_CODE_VA and USER_STACK_VA are both slot 1 but different PDPT indices:
       1 PDPT + 2 PD + 2 PT = 5 private tables. Clone slot 0 from master. */
    uint64_t* root = (uint64_t*)pt_stub_ops.alloc();
    CHECK(root != NULL, "root allocation failed");
    if (!root) return;

    pt_clone_shared(master, root);
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "code map failed");
    CHECK(pt_map(root, USER_STACK_VA, 0x2000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "stack map failed");
    CHECK(pt_stub_alloc_count() == 6, "expected root + 5 tables, got %d",
          pt_stub_alloc_count());
    CHECK(pt_count_user_pages(root) == 2, "setup mapped %d pages",
          pt_count_user_pages(root));

    pt_free_user_tables(root, &pt_stub_ops);
    CHECK(pt_stub_freed_count() == 5, "expected 5 private tables, got %d",
          pt_stub_freed_count());
    CHECK(root[0] == master[0], "shared slot cleared");
    CHECK(pt_count_user_pages(root) == 0, "user leaves remain");
}

static void test_free_idempotent(void) {
    printf("\n=== Test 10: repeated teardown frees nothing ===\n");
    pt_stub_reset();
    uint64_t* master = pt_stub_root();
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;
    uint64_t* root = (uint64_t*)pt_stub_ops.alloc();
    CHECK(root != NULL, "root allocation failed");
    if (!root) return;

    pt_clone_shared(master, root);
    CHECK(pt_map(root, USER_CODE_VA, 0x1000000,
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "setup map failed");
    pt_free_user_tables(root, &pt_stub_ops);
    CHECK(pt_stub_freed_count() == 3, "first teardown freed %d tables",
          pt_stub_freed_count());

    /* Ledger only: root and its (now unlinked) tables stay as they are. */
    pt_stub_reset_freed_count();
    pt_free_user_tables(root, &pt_stub_ops);
    CHECK(pt_stub_freed_count() == 0, "second free released pages");
}

int main(void) {
    printf("ARC OS page-table core test (real kernel/pagetable.c)\n");

    test_map_and_lookup();
    test_intermediate_reuse();
    test_kernel_slot_not_counted();
    test_unmap();
    test_large_page_refused();
    test_alloc_failure();
    test_clone_shared();
    test_isolation();
    test_free_user_tables();
    test_free_idempotent();

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
