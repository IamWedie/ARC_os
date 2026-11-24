/*
 * ARC OS user-memory boundary test.
 *
 * Links the REAL kernel/user_access.c and kernel/pagetable.c against
 * tests/pt_stub.c. The stub supplies only page-table memory; every validation
 * decision is made by the production helper walking a real PML4 table.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Host page source and process globals required by the production helper. */
extern void pt_stub_reset(void);
extern uint64_t* pt_stub_root(void);
extern pt_ops_t pt_stub_ops;

process_t processes[MAX_PROCESSES];
volatile int current_process;

#define KERNEL_TEST_VA       0x0000000000200000ULL /* PML4 slot 0 */
#define USER_GOOD_VA         0x0000008000000000ULL /* PML4 slot 1, RW user */
#define CROSS_PAGE_VA        0x0000008000000FFCULL /* good page + unmapped next page */
#define USER_UNMAPPED_VA     0x0000008000001000ULL /* no leaf mapping */
#define USER_RO_VA           0x0000009000000000ULL /* present/user, not RW */
#define USER_UNTERMINATED_VA 0x0000009000001000ULL /* filled with non-NUL bytes */
#define USER_STRING_VA       0x0000009000002000ULL /* NUL-terminated string */
#define USER_WRITE_VA        0x0000009000003000ULL /* RW user write target */

static uint8_t user_good[4096] __attribute__((aligned(4096)));
static uint8_t user_ro[4096] __attribute__((aligned(4096)));
static uint8_t user_unterminated[4096] __attribute__((aligned(4096)));
static uint8_t user_string[4096] __attribute__((aligned(4096)));
static uint8_t user_write[4096] __attribute__((aligned(4096)));
static uint8_t kernel_page[4096] __attribute__((aligned(4096)));

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

static uint64_t page_phys(const void* page) {
    return (uint64_t)(uintptr_t)page;
}

static void fill_sentinel(void* buf, size_t len) {
    memset(buf, 0x5A, len);
}

static int all_bytes(const void* buf, size_t len, uint8_t value) {
    const uint8_t* p = (const uint8_t*)buf;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != value) return 0;
    }
    return 1;
}

static void setup_world(void) {
    pt_stub_reset();
    memset(processes, 0, sizeof(processes));
    current_process = 0;

    memset(user_good, 0, sizeof(user_good));
    for (size_t i = 0; i < 16; i++) user_good[i] = (uint8_t)(0x10 + i);
    for (size_t i = 0; i < 4; i++) user_good[4092 + i] = (uint8_t)(0xA0 + i);

    memset(user_ro, 0, sizeof(user_ro));
    user_ro[0] = 0xAA;

    memset(user_unterminated, 'A', sizeof(user_unterminated));

    memset(user_string, 0, sizeof(user_string));
    memcpy(user_string, "abc", 3);

    memset(user_write, 0, sizeof(user_write));

    uint64_t* root = pt_stub_root();
    processes[0].pml4_phys = page_phys(root);

    CHECK(pt_map(root, USER_GOOD_VA, page_phys(user_good),
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "setup: user good page map failed");
    /* Map RW pages first: pt_descend sets intermediate flags only when the
     * table is first allocated, and the 0x9000000000 pages share one PD. */
    CHECK(pt_map(root, USER_UNTERMINATED_VA, page_phys(user_unterminated),
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "setup: unterminated page map failed");
    CHECK(pt_map(root, USER_STRING_VA, page_phys(user_string),
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "setup: string page map failed");
    CHECK(pt_map(root, USER_WRITE_VA, page_phys(user_write),
                 PT_PG_USER | PT_PG_RW, &pt_stub_ops) == 0,
          "setup: writable user page map failed");
    CHECK(pt_map(root, USER_RO_VA, page_phys(user_ro),
                 PT_PG_USER, &pt_stub_ops) == 0,
          "setup: read-only user page map failed");
    CHECK(pt_map(root, KERNEL_TEST_VA, page_phys(kernel_page),
                 PT_PG_RW, &pt_stub_ops) == 0,
          "setup: kernel slot page map failed");
}

static void test_valid_copy(void) {
    printf("\n=== Test 1: valid mapped user copy ===\n");
    uint8_t kernel_buf[16];
    fill_sentinel(kernel_buf, sizeof(kernel_buf));

    CHECK(copy_from_user(kernel_buf, (const void*)USER_GOOD_VA, 16) == 0,
          "valid copy returned failure");
    CHECK(memcmp(kernel_buf, user_good, 16) == 0, "valid bytes differ");
}

static void test_range_validation(void) {
    printf("\n=== Test 2: full-range validation before transfer ===\n");
    CHECK(user_range_readable((const void*)USER_GOOD_VA, 16) == 0,
          "readable range rejected");
    CHECK(user_range_readable((const void*)USER_UNMAPPED_VA, 4) == -1,
          "unmapped readable range accepted");
    CHECK(user_range_readable((const void*)CROSS_PAGE_VA, 8) == -1,
          "cross-page readable range accepted");
    CHECK(user_range_writable((void*)USER_WRITE_VA, 8) == 0,
          "writable range rejected");
    CHECK(user_range_writable((void*)USER_RO_VA, 1) == -1,
          "read-only writable range accepted");
    CHECK(user_range_writable((void*)CROSS_PAGE_VA, 8) == -1,
          "cross-page writable range accepted");
    CHECK(user_range_readable(0, 0) == 0 && user_range_writable(0, 0) == 0,
          "zero-length validation failed");
}

static void test_invalid_copies_do_not_touch_destination(void) {
    printf("\n=== Test 3: invalid copies are rejected before access ===\n");
    uint8_t kernel_buf[16];

    fill_sentinel(kernel_buf, sizeof(kernel_buf));
    CHECK(copy_from_user(kernel_buf, (const void*)USER_UNMAPPED_VA, 4) == -1,
          "unmapped copy accepted");
    CHECK(all_bytes(kernel_buf, sizeof(kernel_buf), 0x5A),
          "unmapped copy changed destination");

    fill_sentinel(kernel_buf, sizeof(kernel_buf));
    CHECK(copy_from_user(kernel_buf, (const void*)KERNEL_TEST_VA, 4) == -1,
          "kernel VA copy accepted");
    CHECK(all_bytes(kernel_buf, sizeof(kernel_buf), 0x5A),
          "kernel VA copy changed destination");

    fill_sentinel(kernel_buf, sizeof(kernel_buf));
    CHECK(copy_from_user(kernel_buf, (const void*)CROSS_PAGE_VA, 8) == -1,
          "cross-page copy accepted");
    CHECK(all_bytes(kernel_buf, sizeof(kernel_buf), 0x5A),
          "cross-page copy changed destination");

    CHECK(user_ro[0] == 0xAA, "read-only destination setup changed");
    CHECK(copy_to_user((void*)USER_RO_VA, "x", 1) == -1,
          "read-only copy-out accepted");
    CHECK(user_ro[0] == 0xAA, "read-only destination was written");

    fill_sentinel(kernel_buf, sizeof(kernel_buf));
    CHECK(copy_from_user(kernel_buf, (const void*)USER_GOOD_VA, SIZE_MAX) == -1,
          "overflowing copy accepted");
    CHECK(all_bytes(kernel_buf, sizeof(kernel_buf), 0x5A),
          "overflowing copy changed destination");
}

static void test_zero_length_and_valid_write(void) {
    printf("\n=== Test 4: zero length and valid copy-out ===\n");
    CHECK(copy_from_user(0, 0, 0) == 0, "zero-length from-user failed");
    CHECK(copy_to_user(0, 0, 0) == 0, "zero-length to-user failed");

    memset(user_write, 0, sizeof(user_write));
    CHECK(copy_to_user((void*)USER_WRITE_VA, "xyz", 3) == 0,
          "valid copy-out failed");
    CHECK(memcmp(user_write, "xyz", 3) == 0, "valid copy-out bytes differ");
}

static void test_user_string(void) {
    printf("\n=== Test 5: bounded user string copy ===\n");
    char buf[8];

    fill_sentinel(buf, sizeof(buf));
    CHECK(copy_user_string((const void*)USER_STRING_VA, buf, sizeof(buf)) == 3,
          "terminated string length wrong");
    CHECK(strcmp(buf, "abc") == 0, "terminated string bytes wrong");

    fill_sentinel(buf, sizeof(buf));
    CHECK(copy_user_string((const void*)USER_UNTERMINATED_VA, buf,
                           sizeof(buf)) == -1,
          "unterminated string accepted");
    CHECK(all_bytes(buf, sizeof(buf), 0x5A),
          "unterminated string changed destination");
}

int main(void) {
    printf("ARC OS user-access boundary test\n");
    setup_world();
    test_valid_copy();
    test_range_validation();
    test_invalid_copies_do_not_touch_destination();
    test_zero_length_and_valid_write();
    test_user_string();

    printf("\nUser-access test summary: %d passed, %d failed\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
