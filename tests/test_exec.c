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
extern int exec_stub_total_refs(void);
extern unsigned exec_stub_add_ref_calls(void);
extern unsigned exec_stub_release_calls(void);
/* Lets a host test create a slot whose declared size exceeds the loader cap
 * without writing that many bytes into the stub's small data buffer. */
extern int exec_stub_set_slot_size(const char* name, uint32_t size);

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

    /* The RAM filesystem caps images at FS_MAX_FILESIZE; one byte over must
     * be rejected even though it still fits in USER_FRAME_PAGES. */
    static uint8_t big_image[FS_MAX_FILESIZE + 1u];
    memset(big_image, 0x3C, sizeof(big_image));
    memcpy(saved, processes[pid].owned, sizeof(saved));
    used_before_fail = exec_stub_used_pages();
    rsp_bad = 0xFEEDFACEULL;
    exec_stub_cr3_pid = -1;

    CHECK(exec_load_image(pid, big_image, FS_MAX_FILESIZE + 1u,
                          argv, 2, &rsp_bad) == -1,
          "image above FS_MAX_FILESIZE accepted");
    CHECK(processes[pid].pml4_phys == new_root,
          "root changed on FS-limit failure");
    CHECK(memcmp(processes[pid].owned, saved, sizeof(saved)) == 0,
          "owned[] changed on FS-limit failure");
    CHECK(rsp_bad == 0xFEEDFACEULL,
          "user_rsp_out changed on FS-limit failure");
    CHECK(exec_stub_cr3_pid == -1,
          "CR3 reloaded on FS-limit failure (got %d)", exec_stub_cr3_pid);
    CHECK(exec_stub_used_pages() == used_before_fail,
          "PMM changed on FS-limit failure: %llu -> %llu",
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

/* ------------------------------------------------------------------ */
/* Tests 3-7: SYS_EXEC body and dispatch                               */
/* ------------------------------------------------------------------ */

#define EXEC_TEST_ARG_VA   0xA000000000ULL
#define EXEC_TEST_ARGV_VA  (EXEC_TEST_ARG_VA + 0x100u)
#define EXEC_TEST_STR0_VA  (EXEC_TEST_ARG_VA + 0x200u)
#define EXEC_TEST_STR1_VA  (EXEC_TEST_ARG_VA + 0x300u)
#define EXEC_TEST_BAD_VA   0xB000000000ULL
#define EXEC_TEST_KERN_VA  0xC000000000ULL

typedef struct {
    int pid;
    uint8_t* arg_page;
    uint64_t kstack;
    syscall_frame_t frame;
    fd_entry_t std_fds[3];
} exec_world_t;

static uint8_t exec_test_image[64];

static void* exec_test_pt_alloc(void) {
    void* page = pmm_alloc_pages(1);
    if (page) memset(page, 0, 4096);
    return page;
}
static void exec_test_pt_free(void* page) { pmm_free_pages(page, 1); }
static const pt_ops_t exec_test_pt_ops = {
    exec_test_pt_alloc,
    exec_test_pt_free
};

static void exec_fill_image(void) {
    for (size_t i = 0; i < sizeof(exec_test_image); i++)
        exec_test_image[i] = (uint8_t)(i * 11u + 5u);
}

/* One mapped user page holds the path, the argv pointer array and two strings.
 * It is registered as owned[2], so a successful exec frees it with the old
 * image while a failed exec must leave it (and its mapping) intact. */
static int exec_setup_world(exec_world_t* w) {
    reset_world();

    void* kstack = pmm_alloc_pages(4);
    if (!kstack) return -1;
    w->kstack = (uint64_t)(uintptr_t)kstack;
    w->pid = process_create_user(USER_CODE_VA, w->kstack + 4u * 4096u,
                                 "exec-sys");
    if (w->pid <= 0 || as_create(w->pid) != 0) return -1;

    w->arg_page = (uint8_t*)pmm_alloc_pages(1);
    if (!w->arg_page) return -1;
    uint64_t arg_pa = (uint64_t)(uintptr_t)w->arg_page;
    if (as_map_user(w->pid, EXEC_TEST_ARG_VA, arg_pa, 1,
                    PT_PG_RW | PT_PG_USER) != 0) return -1;
    processes[w->pid].owned[2].base = arg_pa;
    processes[w->pid].owned[2].pages = 1;

    current_process = w->pid;
    memset(&w->frame, 0, sizeof(w->frame));
    w->frame.r15 = 0x1111111111111111ULL;
    w->frame.rdi = 0x2222222222222222ULL;
    w->frame.rip = 0x1234567890ABCDEFULL;
    w->frame.cs = GDTSEL_UC | 3;
    w->frame.rflags = 0x202;
    w->frame.user_rsp = 0xABCDEF0123456780ULL;
    w->frame.ss = GDTSEL_UD | 3;
    w->frame.rax = 16;
    w->frame.ret_slot = 0xDEADBEEFCAFEBABEULL;
    syscall_frame_rsp = (uint64_t)(uintptr_t)&w->frame;
    memcpy(w->std_fds, processes[w->pid].fds, sizeof(w->std_fds));
    return 0;
}

static void exec_write_args(exec_world_t* w, const char* path) {
    memset(w->arg_page, 0, 4096);
    strcpy((char*)w->arg_page, path);

    uint64_t* ptrs = (uint64_t*)(void*)(w->arg_page + 0x100);
    ptrs[0] = EXEC_TEST_STR0_VA;
    ptrs[1] = EXEC_TEST_STR1_VA;
    ptrs[2] = 0;
    strcpy((char*)w->arg_page + 0x200, "hello.bin");
    strcpy((char*)w->arg_page + 0x300, "alpha");
}

static void exec_make_image(const char* name) {
    exec_fill_image();
    int slot = fs_open_slot(name, 1);
    CHECK(slot >= 0, "setup fs_open_slot(%s) failed", name);
    if (slot < 0) return;
    CHECK(fs_slot_write(slot, 0, exec_test_image,
                        sizeof(exec_test_image)) == (int)sizeof(exec_test_image),
          "setup image write for %s failed", name);
}

static void exec_teardown_world(exec_world_t* w) {
    for (int fd = 0; fd < FD_MAX; fd++) fd_close(w->pid, fd);
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        if (processes[w->pid].owned[i].base)
            pmm_free_pages((void*)(uintptr_t)processes[w->pid].owned[i].base,
                           (size_t)processes[w->pid].owned[i].pages);
        processes[w->pid].owned[i].base = 0;
        processes[w->pid].owned[i].pages = 0;
    }
    as_destroy(w->pid);
    pmm_free_pages((void*)(uintptr_t)w->kstack, 4);
    current_process = 0;
    syscall_frame_rsp = 0;
    CHECK(exec_stub_used_pages() == 0, "PMM leak after test: %llu",
          (unsigned long long)exec_stub_used_pages());
}

static void check_exec_failure(exec_world_t* w, const char* label,
                               uint64_t path_va, uint64_t argv_va) {
    syscall_frame_t frame_before = w->frame;
    proc_region_t owned_before[PROCESS_MAX_OWNED_REGIONS];
    fd_entry_t fds_before[FD_MAX];
    memcpy(owned_before, processes[w->pid].owned, sizeof(owned_before));
    memcpy(fds_before, processes[w->pid].fds, sizeof(fds_before));
    uint64_t root_before = processes[w->pid].pml4_phys;
    int refs_before = exec_stub_total_refs();
    uint64_t used_before = exec_stub_used_pages();

    CHECK(sys_exec((const char*)(uintptr_t)path_va,
                   (char* const*)(uintptr_t)argv_va) == -1,
          "%s: sys_exec did not fail", label);
    CHECK(memcmp(&w->frame, &frame_before, sizeof(frame_before)) == 0,
          "%s: syscall frame changed", label);
    CHECK(processes[w->pid].pml4_phys == root_before, "%s: root changed", label);
    CHECK(memcmp(processes[w->pid].owned, owned_before,
                 sizeof(owned_before)) == 0,
          "%s: owned runs changed", label);
    CHECK(memcmp(processes[w->pid].fds, fds_before, sizeof(fds_before)) == 0,
          "%s: fd table changed", label);
    CHECK(exec_stub_total_refs() == refs_before, "%s: slot refs changed", label);
    CHECK(exec_stub_used_pages() == used_before, "%s: PMM changed", label);
    CHECK(preempt_disabled == 0, "%s: preemption left disabled", label);
}

static void test_exec_frame_rewrite(void) {
    printf("\n=== Test 3: SYS_EXEC rewrites the live syscall frame ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_write_args(&w, "hello.bin");

    int fd_a = fd_open(w.pid, "dataA.txt", O_CREAT | O_RDWR);
    int fd_b = fd_dup(w.pid, fd_a);      /* same OFD as fd_a */
    int fd_c = fd_open(w.pid, "dataB.txt", O_CREAT | O_RDWR);
    CHECK(fd_a == 3 && fd_b == 4 && fd_c == 5,
          "setup fds %d/%d/%d, want 3/4/5", fd_a, fd_b, fd_c);
    int refs_before = exec_stub_total_refs();
    syscall_frame_t before = w.frame;
    uint64_t root_before = processes[w.pid].pml4_phys;

    CHECK(sys_exec((const char*)(uintptr_t)EXEC_TEST_ARG_VA,
                   (char* const*)(uintptr_t)EXEC_TEST_ARGV_VA) == 0,
          "sys_exec returned nonzero");

    static uint8_t scratch[4096];
    char* expected_argv[] = { "hello.bin", "alpha", NULL };
    uint64_t expected_rsp = exec_build_user_stack(scratch, sizeof(scratch),
                                                  expected_argv, 2);
    CHECK(w.frame.rip == USER_CODE_VA,
          "rip 0x%llx, want 0x%llx", (unsigned long long)w.frame.rip,
          (unsigned long long)USER_CODE_VA);
    CHECK(w.frame.user_rsp == expected_rsp,
          "user_rsp 0x%llx, want loader value 0x%llx",
          (unsigned long long)w.frame.user_rsp,
          (unsigned long long)expected_rsp);
    CHECK(w.frame.cs == before.cs && w.frame.ss == before.ss,
          "cs/ss were rewritten");
    CHECK(w.frame.rflags == before.rflags && w.frame.r15 == before.r15 &&
              w.frame.rdi == before.rdi,
          "unrelated frame fields changed");
    CHECK(w.frame.rax == 0 && w.frame.ret_slot == 0,
          "rax 0x%llx / ret_slot 0x%llx, want 0",
          (unsigned long long)w.frame.rax,
          (unsigned long long)w.frame.ret_slot);

    CHECK(memcmp(processes[w.pid].fds, w.std_fds, sizeof(w.std_fds)) == 0,
          "fds 0/1/2 changed");
    for (int fd = 3; fd < FD_MAX; fd++)
        CHECK(processes[w.pid].fds[fd].kind == FD_ENTRY_FREE,
              "fd %d still open after exec", fd);
    CHECK(exec_stub_total_refs() == refs_before - 2,
          "slot refs %d, want %d (one per OFD)",
          exec_stub_total_refs(), refs_before - 2);
    CHECK(processes[w.pid].pml4_phys != root_before, "root was not replaced");
    CHECK(exec_stub_cr3_pid == w.pid, "CR3 not reloaded (got %d)",
          exec_stub_cr3_pid);
    CHECK(preempt_disabled == 0, "preemption left disabled");

    uint64_t* leaf = pt_lookup((uint64_t*)(uintptr_t)processes[w.pid].pml4_phys,
                               USER_CODE_VA);
    CHECK(leaf != NULL, "USER_CODE_VA unmapped after exec");
    if (leaf) {
        uint8_t* code = (uint8_t*)(uintptr_t)(*leaf & PT_ADDR_MASK);
        CHECK(memcmp(code, exec_test_image, sizeof(exec_test_image)) == 0,
              "loaded code bytes differ from the FS image");
    }

    exec_teardown_world(&w);
}

static void test_exec_failure_is_atomic(void) {
    printf("\n=== Test 4: SYS_EXEC failures leave the image byte-for-byte ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_make_image("huge.bin");
    CHECK(exec_stub_set_slot_size("huge.bin", FS_MAX_FILESIZE + 1u) == 0,
          "setup oversized slot failed");

    exec_write_args(&w, "no-such.bin");
    check_exec_failure(&w, "missing path", EXEC_TEST_ARG_VA, EXEC_TEST_ARGV_VA);

    exec_write_args(&w, "huge.bin");
    check_exec_failure(&w, "oversized image", EXEC_TEST_ARG_VA,
                       EXEC_TEST_ARGV_VA);

    exec_write_args(&w, "hello.bin");
    ((uint64_t*)(void*)(w.arg_page + 0x100))[1] = EXEC_TEST_BAD_VA;
    check_exec_failure(&w, "bad argv pointer", EXEC_TEST_ARG_VA,
                       EXEC_TEST_ARGV_VA);

    exec_teardown_world(&w);
}

static void test_exec_rejects_bad_user_pointer(void) {
    printf("\n=== Test 5: SYS_EXEC rejects unmapped and kernel-only pointers ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_write_args(&w, "hello.bin");

    check_exec_failure(&w, "unmapped path", EXEC_TEST_BAD_VA,
                       EXEC_TEST_ARGV_VA);

    /* Same bytes, but mapped without PT_PG_USER: user mode could not read it. */
    uint8_t* kernel_page = (uint8_t*)pmm_alloc_pages(1);
    CHECK(kernel_page != NULL, "setup kernel-only page failed");
    if (kernel_page) {
        uint64_t pa = (uint64_t)(uintptr_t)kernel_page;
        strcpy((char*)kernel_page, "hello.bin");
        /* as_map_user() always adds PT_PG_USER, so use the raw mapper. */
        CHECK(pt_map((uint64_t*)(uintptr_t)processes[w.pid].pml4_phys,
                     EXEC_TEST_KERN_VA, pa, PT_PG_RW,
                     &exec_test_pt_ops) == 0,
              "setup kernel-only mapping failed");
        processes[w.pid].owned[3].base = pa;
        processes[w.pid].owned[3].pages = 1;
        check_exec_failure(&w, "kernel-only path", EXEC_TEST_KERN_VA,
                           EXEC_TEST_ARGV_VA);
    }

    exec_teardown_world(&w);
}

static void test_exec_close_on_exec(void) {
    printf("\n=== Test 6: SYS_EXEC closes file fds and releases one ref per OFD ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_write_args(&w, "hello.bin");

    int fd_a = fd_open(w.pid, "keepA.txt", O_CREAT | O_RDWR);
    int fd_b = fd_dup(w.pid, fd_a);      /* two fds, one OFD */
    int fd_c = fd_open(w.pid, "keepB.txt", O_CREAT | O_RDWR);
    CHECK(fd_a >= 3 && fd_b >= 3 && fd_c >= 3, "setup file fds failed");
    int refs_before = exec_stub_total_refs();
    CHECK(refs_before == 2, "setup slot refs %d, want 2 one-per-OFD refs",
          refs_before);

    CHECK(sys_exec((const char*)(uintptr_t)EXEC_TEST_ARG_VA,
                   (char* const*)(uintptr_t)EXEC_TEST_ARGV_VA) == 0,
          "sys_exec failed");
    CHECK(exec_stub_total_refs() == 0, "slot refs after exec %d, want 0",
          exec_stub_total_refs());
    CHECK(memcmp(processes[w.pid].fds, w.std_fds, sizeof(w.std_fds)) == 0,
          "fds 0/1/2 changed");
    for (int fd = 3; fd < FD_MAX; fd++)
        CHECK(processes[w.pid].fds[fd].kind == FD_ENTRY_FREE,
              "fd %d survived exec", fd);

    exec_teardown_world(&w);
}

static void test_exec_syscall_dispatch(void) {
    printf("\n=== Test 7: SYS_EXEC (16) dispatch reaches sys_exec ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_write_args(&w, "hello.bin");

    /* Pin the ABI number: no renumbering of syscalls 1-15. */
    CHECK(syscall_dispatch(16, (uint64_t)(uintptr_t)EXEC_TEST_ARG_VA,
                           (uint64_t)(uintptr_t)EXEC_TEST_ARGV_VA, 0) == 0,
          "SYS_EXEC dispatch did not return 0");
    CHECK(w.frame.rip == USER_CODE_VA, "dispatch did not rewrite rip");
    CHECK(w.frame.user_rsp >= USER_STACK_VA && w.frame.user_rsp < USER_STACK_TOP,
          "dispatch user_rsp 0x%llx outside the new stack",
          (unsigned long long)w.frame.user_rsp);
    CHECK(preempt_disabled == 0, "preemption left disabled after dispatch");

    exec_teardown_world(&w);
}

static void test_exec_accepts_eight_args(void) {
    printf("\n=== Test 8: SYS_EXEC accepts exactly EXEC_MAX_ARGS arguments ===\n");
    exec_world_t w;
    if (exec_setup_world(&w) != 0) { CHECK(0, "setup failed"); return; }
    exec_make_image("hello.bin");
    exec_write_args(&w, "hello.bin");

    /* Eight non-NULL entries plus the required NULL at slot 8. */
    static const char* const eight_args[EXEC_MAX_ARGS] = {
        "arg0", "arg1", "arg2", "arg3",
        "arg4", "arg5", "arg6", "arg7"
    };
    char* expected_argv[EXEC_MAX_ARGS + 1];
    uint64_t* ptrs = (uint64_t*)(void*)(w.arg_page + 0x100);
    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
        ptrs[i] = EXEC_TEST_ARG_VA + 0x200u + (uint64_t)i * 0x40u;
        strcpy((char*)w.arg_page + 0x200u + (size_t)i * 0x40u,
               eight_args[i]);
        expected_argv[i] = (char*)eight_args[i];
    }
    ptrs[EXEC_MAX_ARGS] = 0;
    expected_argv[EXEC_MAX_ARGS] = NULL;

    CHECK(sys_exec((const char*)(uintptr_t)EXEC_TEST_ARG_VA,
                   (char* const*)(uintptr_t)EXEC_TEST_ARGV_VA) == 0,
          "sys_exec rejected exactly 8 arguments (argc == EXEC_MAX_ARGS)");
    CHECK(w.frame.rip == USER_CODE_VA, "eight-arg exec did not rewrite rip");

    static uint8_t scratch[4096];
    uint64_t expected_rsp = exec_build_user_stack(scratch, sizeof(scratch),
                                                  expected_argv, EXEC_MAX_ARGS);
    CHECK(w.frame.user_rsp == expected_rsp,
          "user_rsp 0x%llx, want loader value 0x%llx",
          (unsigned long long)w.frame.user_rsp,
          (unsigned long long)expected_rsp);

    if (w.frame.user_rsp >= USER_STACK_VA && w.frame.user_rsp < USER_STACK_TOP) {
        uint64_t off = USER_STACK_TOP - w.frame.user_rsp;
        if (off >= 8 && off <= USER_STACK_PAGES * 4096u -
                                  (EXEC_MAX_ARGS + 2u) * 8u) {
            uint8_t* stack_phys =
                (uint8_t*)(uintptr_t)processes[w.pid].owned[1].base;
            uint64_t* words = (uint64_t*)(void*)(stack_phys +
                                        USER_STACK_PAGES * 4096u - (size_t)off);
            CHECK(words[0] == EXEC_MAX_ARGS, "argc word %llu, want 8",
                  (unsigned long long)words[0]);
            CHECK(words[EXEC_MAX_ARGS] != 0 &&
                      words[EXEC_MAX_ARGS + 1] == 0,
                  "argv table is not terminated after 8 entries");
        }
    }

    exec_teardown_world(&w);
}

/* ------------------------------------------------------------------ */
/* Test 9: boot-time program registry                                   */
/* ------------------------------------------------------------------ */

static void test_exec_install_programs(void) {
    printf("\n=== Test 9: exec_install_programs installs the embedded hello blob ===\n");
    reset_world();

    exec_install_programs();

    int slot = fs_find_slot("hello.bin");
    CHECK(slot >= 0, "hello.bin was not installed in the FS");
    if (slot < 0) return;

    size_t blob_size = (size_t)_binary_user_hello_bin_size;
    CHECK(blob_size == 6, "fake blob size %u, want 6", (unsigned)blob_size);
    CHECK(fs_slot_size(slot) == (int)blob_size,
          "hello.bin size %d, want %u", fs_slot_size(slot), (unsigned)blob_size);

    uint8_t buf[64] = {0};
    size_t read_size = blob_size < sizeof(buf) ? blob_size : sizeof(buf);
    CHECK(fs_slot_read(slot, 0, buf, read_size) == (int)read_size,
          "fs_slot_read did not return the whole blob");
    CHECK(memcmp(buf, _binary_user_hello_bin_start, read_size) == 0,
          "hello.bin bytes differ from the embedded blob");

    CHECK(exec_stub_add_ref_calls() == 1,
          "fs_slot_add_ref called %u times, want 1",
          exec_stub_add_ref_calls());
    CHECK(exec_stub_release_calls() == 1,
          "fs_slot_release called %u times, want 1",
          exec_stub_release_calls());
    CHECK(exec_stub_total_refs() == 0,
          "boot registry leaked %d slot refs", exec_stub_total_refs());
}

/* ------------------------------------------------------------------ */
/* Test 10: boot registry replaces a stale persisted hello.bin          */
/* ------------------------------------------------------------------ */

static void test_exec_install_programs_truncates_stale_slot(void) {
    printf("\n=== Test 10: exec_install_programs truncates a stale hello.bin ===\n");
    reset_world();

    /* Simulate a persisted slot written by an older, larger blob. */
    int stale = fs_open_slot("hello.bin", 1);
    CHECK(stale >= 0, "setup fs_open_slot for stale hello.bin failed");
    if (stale < 0) return;
    CHECK(fs_slot_add_ref(stale) == 0, "setup add_ref failed");
    uint8_t old_data[32];
    memset(old_data, 0xCC, sizeof(old_data));
    CHECK(fs_slot_write(stale, 0, old_data, sizeof(old_data)) ==
              (int)sizeof(old_data),
          "setup stale write failed");
    fs_slot_release(stale);
    CHECK(fs_slot_size(stale) == (int)sizeof(old_data),
          "setup stale size %d, want %u", fs_slot_size(stale),
          (unsigned)sizeof(old_data));

    exec_install_programs();

    size_t blob_size = (size_t)_binary_user_hello_bin_size;
    CHECK(fs_slot_size(stale) == (int)blob_size,
          "hello.bin size %d, want %u after reinstall",
          fs_slot_size(stale), (unsigned)blob_size);

    uint8_t tail[8] = {0};
    int n = fs_slot_read(stale, (uint32_t)blob_size, tail, sizeof(tail));
    CHECK(n == 0, "read past reinstalled blob returned %d stale bytes", n);
    for (size_t i = 0; i < sizeof(tail); i++)
        CHECK(tail[i] == 0, "stale byte %u = 0x%02x at offset %u",
              (unsigned)i, tail[i], (unsigned)(blob_size + i));
    CHECK(exec_stub_total_refs() == 0, "reinstall leaked %d slot refs",
          exec_stub_total_refs());
}

int main(void) {
    printf("ARC OS exec-loader test (real kernel sources)\n");

    test_exec_stack_layout();
    test_exec_load_image();
    test_exec_frame_rewrite();
    test_exec_failure_is_atomic();
    test_exec_rejects_bad_user_pointer();
    test_exec_close_on_exec();
    test_exec_syscall_dispatch();
    test_exec_accepts_eight_args();
    test_exec_install_programs();
    test_exec_install_programs_truncates_stale_slot();

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
