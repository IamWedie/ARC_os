/*
 * ARC OS exec-loader host test.
 *
 * Links against the REAL kernel/scheduler.c, address_space.c, pagetable.c,
 * exec.c and fd.c with tests/exec_stub.c supplying the hardware and service
 * hooks. Proves the argc/argv stack layout and the failure-atomic image swap
 * without ever loading CR3 or executing privileged instructions on the host.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Page-table leaves carry flags in their low bits. */
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

extern uint64_t exec_stub_used_pages(void);
extern void exec_stub_reset(void);
extern int exec_stub_cr3_pid;

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

static void reset_world(void) {
    exec_stub_reset();
    scheduler_init();
    exec_stub_cr3_pid = -1;
}

/* ------------------------------------------------------------------ */
/* Test 1: stack layout                                                */
/* ------------------------------------------------------------------ */

static uint8_t stack_buf[4096] __attribute__((aligned(16)));

/* exec_build_user_stack returns user VAs, so translate back into the host
 * buffer using the same top-relative offset the kernel uses. */
static uint8_t* host_from_user_va(uint64_t va) {
    if (va < USER_STACK_VA || va >= USER_STACK_TOP) return NULL;
    uint64_t off = USER_STACK_TOP - va;
    if (off > sizeof(stack_buf)) return NULL;
    return stack_buf + sizeof(stack_buf) - (size_t)off;
}

static void check_arg_string(uint64_t va, const char* expected, const char* label) {
    uint8_t* s = host_from_user_va(va);
    CHECK(s != NULL, "%s VA 0x%llx outside the stack buffer", label,
          (unsigned long long)va);
    if (!s) return;

    const uint8_t* end = stack_buf + sizeof(stack_buf);
    size_t n = 0;
    while (s + n < end && s[n] != '\0') n++;
    CHECK(s + n < end, "%s string is not NUL-terminated", label);
    if (s + n == end) return;
    CHECK(strcmp((const char*)s, expected) == 0, "%s = '%s', want '%s'",
          label, (const char*)s, expected);
}

static void test_exec_stack_layout(void) {
    printf("\n=== Test 1: exec_build_user_stack writes argc/argv top-down ===\n");
    reset_world();
    memset(stack_buf, 0, sizeof(stack_buf));

    char* argv[] = { "hello.bin", "alpha", NULL };
    uint64_t rsp = exec_build_user_stack(stack_buf, sizeof(stack_buf), argv, 2);

    CHECK(rsp >= USER_STACK_VA && rsp < USER_STACK_TOP,
          "rsp 0x%llx outside [USER_STACK_VA, USER_STACK_TOP)",
          (unsigned long long)rsp);
    CHECK(((rsp - USER_STACK_VA) & 7u) == 0, "rsp is not 8-byte aligned");

    size_t argc_off = (size_t)(USER_STACK_TOP - rsp);
    CHECK(argc_off >= 8 && argc_off <= sizeof(stack_buf),
          "argc offset %u outside the 4 KiB buffer", (unsigned)argc_off);
    if (argc_off < 8 || argc_off > sizeof(stack_buf)) return;

    uint64_t* words = (uint64_t*)(void*)(stack_buf + sizeof(stack_buf) - argc_off);
    CHECK(words[0] == 2, "argc = %llu, want 2", (unsigned long long)words[0]);
    CHECK(words[1] != 0, "argv[0] is NULL");
    CHECK(words[2] != 0, "argv[1] is NULL");
    CHECK(words[3] == 0, "argv[2] is not NULL");

    check_arg_string(words[1], "hello.bin", "argv[0]");
    check_arg_string(words[2], "alpha", "argv[1]");
}

/* ------------------------------------------------------------------ */
/* Test 2: image load and failure atomicity                            */
/* ------------------------------------------------------------------ */

static void test_exec_load_image(void) {
    printf("\n=== Test 2: exec_load_image swaps code/stack and rolls back ===\n");
    reset_world();
    uint64_t before = exec_stub_used_pages();

    void* kstack = pmm_alloc_pages(4);
    int pid = process_create_user(USER_CODE_VA,
                                  (uint64_t)(uintptr_t)kstack + 4 * 4096ULL,
                                  "exec-target");
    CHECK(pid > 0, "process_create_user failed (pid %d)", pid);
    if (pid <= 0) return;
    CHECK(as_create(pid) == 0, "setup as_create failed");

    /* Give the process two owned runs with known bytes, mapped in its root. */
    uint8_t* old_code = (uint8_t*)pmm_alloc_pages(2);
    uint8_t* old_stack = (uint8_t*)pmm_alloc_pages(USER_STACK_PAGES);
    CHECK(old_code != NULL && old_stack != NULL, "setup allocation failed");
    if (!old_code || !old_stack) return;
    memset(old_code, 0xA5, 2 * 4096);
    memset(old_stack, 0x5A, USER_STACK_PAGES * 4096);

    processes[pid].owned[0].base = (uint64_t)(uintptr_t)old_code;
    processes[pid].owned[0].pages = 2;
    processes[pid].owned[1].base = (uint64_t)(uintptr_t)old_stack;
    processes[pid].owned[1].pages = USER_STACK_PAGES;
    CHECK(as_map_user(pid, USER_CODE_VA, (uint64_t)(uintptr_t)old_code, 2,
                      PT_PG_RW) == 0, "setup code map failed");
    CHECK(as_map_user(pid, USER_STACK_VA, (uint64_t)(uintptr_t)old_stack,
                      USER_STACK_PAGES, PT_PG_RW) == 0,
          "setup stack map failed");
    uint64_t old_root = processes[pid].pml4_phys;

    /* Synthetic flat image: a little over one page, with recognizable bytes. */
    static uint8_t image[300];
    for (size_t i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 7u + 3u);
    char* argv[] = { "hello.bin", "alpha", NULL };
    uint64_t rsp = 0;

    CHECK(exec_load_image(pid, image, sizeof(image), argv, 2, &rsp) == 0,
          "exec_load_image failed");

    uint64_t new_root = processes[pid].pml4_phys;
    CHECK(new_root != 0 && new_root != old_root, "root was not replaced");
    CHECK(exec_stub_cr3_pid == pid, "CR3 not reloaded for pid %d (got %d)",
          pid, exec_stub_cr3_pid);
    CHECK(processes[pid].owned[0].pages == 1 &&
              processes[pid].owned[0].base != (uint64_t)(uintptr_t)old_code,
          "new code run not registered in owned[0]");
    CHECK(processes[pid].owned[1].pages == USER_STACK_PAGES &&
              processes[pid].owned[1].base != (uint64_t)(uintptr_t)old_stack,
          "new stack run not registered in owned[1]");
    CHECK(processes[pid].owned[2].base == 0 && processes[pid].owned[3].base == 0,
          "leftover owned slots were not cleared");

    /* New code bytes are visible through the new root at USER_CODE_VA. */
    uint64_t* code_leaf = pt_lookup((uint64_t*)(uintptr_t)new_root, USER_CODE_VA);
    CHECK(code_leaf != NULL, "USER_CODE_VA unmapped in the new root");
    if (code_leaf) {
        uint8_t* code_phys = (uint8_t*)(uintptr_t)(*code_leaf & PT_ADDR_MASK);
        CHECK(memcmp(code_phys, image, sizeof(image)) == 0,
              "code bytes at USER_CODE_VA differ from the image");
    }

    /* New stack: rsp is in range and argc/argv are readable there. */
    CHECK(rsp >= USER_STACK_VA && rsp < USER_STACK_TOP,
          "rsp 0x%llx outside the new stack range", (unsigned long long)rsp);
    size_t off = (size_t)(USER_STACK_TOP - rsp);
    CHECK(off >= 8 && off <= USER_STACK_PAGES * 4096u,
          "argc offset %u outside the new stack", (unsigned)off);
    if (off >= 8 && off <= USER_STACK_PAGES * 4096u) {
        uint8_t* stack_phys = (uint8_t*)(uintptr_t)processes[pid].owned[1].base;
        uint64_t* words = (uint64_t*)(void*)(stack_phys +
                                            USER_STACK_PAGES * 4096u - off);
        CHECK(words[0] == 2, "argc = %llu, want 2", (unsigned long long)words[0]);
        CHECK(words[1] != 0 && words[2] != 0 && words[3] == 0,
              "argv pointer table is malformed");
        if (words[1] >= USER_STACK_VA && words[1] < USER_STACK_TOP) {
            uint8_t* s0 = stack_phys + (size_t)(words[1] - USER_STACK_VA);
            CHECK(strcmp((const char*)s0, "hello.bin") == 0,
                  "argv[0] = '%s', want 'hello.bin'", (const char*)s0);
        } else {
            CHECK(0, "argv[0] is not a user VA");
        }
        if (words[2] >= USER_STACK_VA && words[2] < USER_STACK_TOP) {
            uint8_t* s1 = stack_phys + (size_t)(words[2] - USER_STACK_VA);
            CHECK(strcmp((const char*)s1, "alpha") == 0,
                  "argv[1] = '%s', want 'alpha'", (const char*)s1);
        } else {
            CHECK(0, "argv[1] is not a user VA");
        }
    }

    /* Oversized image: -1 with root, owned runs, output and PMM untouched. */
    proc_region_t saved[PROCESS_MAX_OWNED_REGIONS];
    memcpy(saved, processes[pid].owned, sizeof(saved));
    uint64_t used_before_fail = exec_stub_used_pages();
    uint64_t rsp_bad = 0xFEEDFACEULL;
    exec_stub_cr3_pid = -1;

    CHECK(exec_load_image(pid, image, (size_t)USER_FRAME_PAGES * 4096u + 1u,
                          argv, 2, &rsp_bad) == -1,
          "oversized image accepted");
    CHECK(processes[pid].pml4_phys == new_root, "root changed on failure");
    CHECK(memcmp(processes[pid].owned, saved, sizeof(saved)) == 0,
          "owned[] changed on failure");
    CHECK(rsp_bad == 0xFEEDFACEULL, "user_rsp_out changed on failure");
    CHECK(exec_stub_cr3_pid == -1, "CR3 reloaded on failure (got %d)",
          exec_stub_cr3_pid);
    CHECK(exec_stub_used_pages() == used_before_fail,
          "PMM changed on failure: %llu -> %llu",
          (unsigned long long)used_before_fail,
          (unsigned long long)exec_stub_used_pages());

    /* Teardown: released frames and page tables bring the PMM back to zero. */
    pmm_free_pages((void*)(uintptr_t)processes[pid].owned[0].base,
                   (size_t)processes[pid].owned[0].pages);
    pmm_free_pages((void*)(uintptr_t)processes[pid].owned[1].base,
                   (size_t)processes[pid].owned[1].pages);
    for (int i = 0; i < 2; i++) {
        processes[pid].owned[i].base = 0;
        processes[pid].owned[i].pages = 0;
    }
    as_destroy(pid);
    pmm_free_pages(kstack, 4);
    CHECK(exec_stub_used_pages() == before, "PMM leak: %llu",
          (unsigned long long)(exec_stub_used_pages() - before));
}

int main(void) {
    printf("ARC OS exec-loader test (real kernel sources)\n");

    test_exec_stack_layout();
    test_exec_load_image();

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
