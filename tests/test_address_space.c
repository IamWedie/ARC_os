/*
 * ARC OS Per-Process Address-Space Test
 *
 * Links against the REAL kernel/address_space.c and kernel/pagetable.c with
 * host stubs for the PMM, the master root, and the invalidate hook. This is
 * where private-root isolation and PMM net-zero teardown are proven without
 * ever loading CR3 or executing privileged instructions on the host.
 */
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Service under test (not in kernel.h until its kernel callers land). */
extern int as_create(int pid);
extern int as_map_user(int pid, uint64_t vaddr, uint64_t paddr,
                       size_t pages, uint8_t flags);
extern void as_destroy(int pid);
extern uint64_t as_root(int pid);

/* Host stubs / introspection. */
extern void pmm_stub_reset(void);
extern uint64_t pmm_stub_used_pages(void);

process_t processes[MAX_PROCESSES];

/* Fake master root: only slot 0 (the shared kernel map) is populated. */
static uint64_t master[512] __attribute__((aligned(4096)));

uint64_t vmm_master_root(void) {
    return (uint64_t)(uintptr_t)master;
}

void vmm_invalidate_page(uint64_t vaddr) {
    (void)vaddr;
}

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

/* Every test starts from an empty process table and an empty PMM ledger. */
static void reset_world(void) {
    pmm_stub_reset();
    for (int i = 0; i < MAX_PROCESSES; i++) processes[i].pml4_phys = 0;
    for (int i = 0; i < 512; i++) master[i] = 0;
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;
}

static void test_create_isolated(void) {
    printf("\n=== Test 1: private roots share only the kernel slot ===\n");
    reset_world();
    CHECK(as_create(1) == 0, "create 1 failed");
    CHECK(as_create(2) == 0, "create 2 failed");
    CHECK(as_root(1) != 0 && as_root(1) != as_root(2), "roots not private");
    CHECK(((uint64_t*)as_root(1))[0] == master[0], "kernel slot not shared");
    CHECK(((uint64_t*)as_root(1))[PT_USER_SLOT_FIRST] == 0, "user slot not empty");
    CHECK(as_root(0) == 0, "idle root should be master");
    CHECK(as_create(1) == -1, "double create accepted");
}

static void test_map_user_rejects_kernel_va(void) {
    printf("\n=== Test 2: user mapping rejects slot-0 VAs ===\n");
    reset_world();
    CHECK(as_create(1) == 0, "setup create failed");
    CHECK(as_map_user(1, 0x1000, 0x1000000, 1, PT_PG_RW) == -1,
          "kernel VA accepted");
    CHECK(as_map_user(1, USER_CODE_VA, 0x1000000, 1, PT_PG_RW) == 0,
          "user VA rejected");
}

static void test_destroy_idempotent(void) {
    printf("\n=== Test 3: teardown clears the root once ===\n");
    reset_world();
    CHECK(as_create(1) == 0, "setup create failed");
    as_destroy(1);
    CHECK(as_root(1) == 0, "root not cleared");
    as_destroy(1);                               /* must not double-free */
}

static void test_repeated_cycles_net_zero(void) {
    printf("\n=== Test 4: create/map/destroy cycles are PMM net-zero ===\n");
    reset_world();
    uint64_t before = pmm_stub_used_pages();
    for (int i = 0; i < 8; i++) {
        CHECK(as_create(1) == 0, "cycle %d create failed", i);
        CHECK(as_map_user(1, USER_CODE_VA, 0x1000000, 2, PT_PG_RW) == 0,
              "cycle %d map failed", i);
        as_destroy(1);
    }
    CHECK(pmm_stub_used_pages() == before, "PMM leak: %llu",
          (unsigned long long)pmm_stub_used_pages());
}

int main(void) {
    printf("ARC OS per-process address-space test (real kernel sources)\n");

    test_create_isolated();
    test_map_user_rejects_kernel_va();
    test_destroy_idempotent();
    test_repeated_cycles_net_zero();

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
