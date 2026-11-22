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

/* Host stubs / introspection (service API comes from kernel.h). */
extern void pmm_stub_reset(void);
extern uint64_t pmm_stub_used_pages(void);

/* Table entries point at 4 KiB pages; the low bits carry the flags. */
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

process_t processes[MAX_PROCESSES];

/* Fake master root: only slot 0 (the shared kernel map) is populated. */
static uint64_t master[512] __attribute__((aligned(4096)));

/* Real vmm.c loads CR3; the host stub only records which pid was selected. */
static int as_stub_cr3_pid = -1;

uint64_t vmm_master_root(void) {
    return (uint64_t)(uintptr_t)master;
}

void vmm_switch_address_space(int pid) {
    as_stub_cr3_pid = pid;
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
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pml4_phys = 0;
        for (int j = 0; j < PROCESS_MAX_OWNED_REGIONS; j++) {
            processes[i].owned[j].base = 0;
            processes[i].owned[j].pages = 0;
        }
    }
    for (int i = 0; i < 512; i++) master[i] = 0;
    master[0] = 0xABC000ULL | PT_PG_PRESENT | PT_PG_RW;
    as_stub_cr3_pid = -1;
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

static void test_clone_deep_copies_user_pages(void) {
    printf("\n=== Test 5: as_clone deep-copies user pages ===\n");
    reset_world();
    uint64_t before = pmm_stub_used_pages();

    CHECK(as_clone(0, 2) == -1, "invalid source pid accepted");
    CHECK(as_clone(1, MAX_PROCESSES) == -1, "invalid destination pid accepted");
    CHECK(as_clone(1, 2) == -1, "clone with absent source root accepted");

    CHECK(as_create(1) == 0, "setup create failed");
    CHECK(as_clone(1, 1) == -1, "clone onto the same pid accepted");

    uint8_t* src_pages = (uint8_t*)pmm_alloc_pages(2);
    CHECK(src_pages != NULL, "source frame allocation failed");
    if (!src_pages) return;
    src_pages[0] = 0x11;
    src_pages[1] = 0x22;
    src_pages[4096] = 0x33;
    src_pages[4097] = 0x44;
    CHECK(as_map_user(1, USER_CODE_VA, (uint64_t)(uintptr_t)src_pages, 2,
                      PT_PG_RW) == 0, "source map failed");

    /* Act: clone the source address space into a fresh process slot. */
    CHECK(as_clone(1, 2) == 0, "clone failed");
    uint64_t dst_root = as_root(2);
    CHECK(dst_root != 0 && dst_root != as_root(1), "clone root missing/shared");
    CHECK(pt_count_user_pages((const uint64_t*)dst_root) == 2,
          "clone page count != 2");

    uint64_t* src_leaf0 = pt_lookup((uint64_t*)as_root(1), USER_CODE_VA);
    uint64_t* dst_leaf0 = pt_lookup((uint64_t*)dst_root, USER_CODE_VA);
    uint64_t* src_leaf1 = pt_lookup((uint64_t*)as_root(1), USER_CODE_VA + 4096);
    uint64_t* dst_leaf1 = pt_lookup((uint64_t*)dst_root, USER_CODE_VA + 4096);
    CHECK(src_leaf0 && dst_leaf0 && src_leaf1 && dst_leaf1, "clone leaf missing");
    if (src_leaf0 && dst_leaf0 && src_leaf1 && dst_leaf1) {
        uint64_t src_pa0 = *src_leaf0 & PT_ADDR_MASK;
        uint64_t dst_pa0 = *dst_leaf0 & PT_ADDR_MASK;
        uint64_t src_pa1 = *src_leaf1 & PT_ADDR_MASK;
        uint64_t dst_pa1 = *dst_leaf1 & PT_ADDR_MASK;
        CHECK(src_pa0 != dst_pa0 && src_pa1 != dst_pa1,
              "clone reused source frames");
        CHECK(dst_pa1 == dst_pa0 + 4096, "clone run is not contiguous");

        uint8_t* dst0 = (uint8_t*)(uintptr_t)dst_pa0;
        uint8_t* dst1 = (uint8_t*)(uintptr_t)dst_pa1;
        CHECK(dst0[0] == 0x11 && dst0[1] == 0x22 &&
              dst1[0] == 0x33 && dst1[1] == 0x44,
              "clone contents differ from source");
        CHECK(processes[2].owned[0].base == dst_pa0 &&
              processes[2].owned[0].pages == 2,
              "clone frames not registered in owned[0]");

        src_pages[0] = 0x99;
        CHECK(dst0[0] == 0x11, "source write changed the child page");

        as_destroy(1);
        CHECK(pt_lookup((uint64_t*)dst_root, USER_CODE_VA) != NULL &&
              pt_lookup((uint64_t*)dst_root, USER_CODE_VA + 4096) != NULL,
              "destroying source unmapped the child");
        CHECK(dst0[0] == 0x11 && dst1[0] == 0x33,
              "destroying source changed child contents");
    }

    as_destroy(2);
    pmm_free_pages((void*)(uintptr_t)processes[2].owned[0].base,
                   (size_t)processes[2].owned[0].pages);
    processes[2].owned[0].base = 0;
    processes[2].owned[0].pages = 0;
    pmm_free_pages(src_pages, 2);
    CHECK(pmm_stub_used_pages() == before, "clone PMM leak: %llu",
          (unsigned long long)pmm_stub_used_pages());
}

static void test_replace_user_swaps_root(void) {
    printf("\n=== Test 6: replace_user installs a fresh detached root ===\n");
    reset_world();
    uint64_t before = pmm_stub_used_pages();
    const uint64_t old_code_pa = 0x1000000ULL;
    const uint64_t old_stack_pa = 0x2000000ULL;
    const uint64_t code_pa = 0x3000000ULL;
    const uint64_t stack_pa = 0x4000000ULL;

    CHECK(as_create(1) == 0, "setup create failed");
    CHECK(as_map_user(1, USER_CODE_VA, old_code_pa, 2, PT_PG_RW) == 0,
          "setup code map failed");
    CHECK(as_map_user(1, USER_STACK_VA, old_stack_pa, 1, PT_PG_RW) == 0,
          "setup stack map failed");
    uint64_t old_root = processes[1].pml4_phys;
    uint64_t old_slot0 = ((uint64_t*)old_root)[0];

    CHECK(as_replace_user(1, code_pa, 2, stack_pa, 1) == 0,
          "replace_user failed");
    uint64_t new_root = processes[1].pml4_phys;
    CHECK(new_root != 0 && new_root != old_root, "root was not replaced");

    uint64_t* code0 = pt_lookup((uint64_t*)new_root, USER_CODE_VA);
    uint64_t* code1 = pt_lookup((uint64_t*)new_root, USER_CODE_VA + 4096);
    uint64_t* stack = pt_lookup((uint64_t*)new_root, USER_STACK_VA);
    CHECK(code0 && (*code0 & PT_ADDR_MASK) == code_pa,
          "new code page 0 has wrong address");
    CHECK(code1 && (*code1 & PT_ADDR_MASK) == code_pa + 4096,
          "new code page 1 has wrong address");
    CHECK(stack && (*stack & PT_ADDR_MASK) == stack_pa,
          "new stack page has wrong address");
    CHECK(((uint64_t*)new_root)[0] == old_slot0,
          "new root lost the shared kernel slot");
    CHECK(as_stub_cr3_pid == 1, "CR3 not reloaded for pid 1 (got %d)",
          as_stub_cr3_pid);

    as_destroy(1);
    CHECK(pmm_stub_used_pages() == before, "replace PMM leak: %llu",
          (unsigned long long)pmm_stub_used_pages());
}

static void test_replace_user_failure_is_atomic(void) {
    printf("\n=== Test 7: replace_user failure leaves the old root intact ===\n");
    reset_world();

    CHECK(as_create(1) == 0, "setup create failed");
    CHECK(as_map_user(1, USER_CODE_VA, 0x1000000ULL, 2, PT_PG_RW) == 0,
          "setup code map failed");
    CHECK(as_map_user(1, USER_STACK_VA, 0x2000000ULL, 1, PT_PG_RW) == 0,
          "setup stack map failed");
    uint64_t old_root = processes[1].pml4_phys;
    uint64_t before = pmm_stub_used_pages();
    processes[1].owned[0].base = 0x12345000ULL;
    processes[1].owned[0].pages = 3;

    CHECK(as_replace_user(1, 0, 2, 0x2000000ULL, 1) == -1,
          "zero code_pa accepted");
    CHECK(processes[1].pml4_phys == old_root, "root changed on failure");
    uint64_t* code = pt_lookup((uint64_t*)old_root, USER_CODE_VA);
    CHECK(code && (*code & PT_ADDR_MASK) == 0x1000000ULL,
          "old mapping lost on failure");
    CHECK(as_stub_cr3_pid == -1, "CR3 reloaded on failure (got %d)",
          as_stub_cr3_pid);
    CHECK(processes[1].owned[0].base == 0x12345000ULL &&
          processes[1].owned[0].pages == 3, "owned[] changed on failure");
    CHECK(pmm_stub_used_pages() == before, "PMM changed on failure: %llu",
          (unsigned long long)pmm_stub_used_pages());

    processes[1].owned[0].base = 0;
    processes[1].owned[0].pages = 0;
    as_destroy(1);
}

int main(void) {
    printf("ARC OS per-process address-space test (real kernel sources)\n");

    test_create_isolated();
    test_map_user_rejects_kernel_va();
    test_destroy_idempotent();
    test_repeated_cycles_net_zero();
    test_clone_deep_copies_user_pages();
    test_replace_user_swaps_root();
    test_replace_user_failure_is_atomic();

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
