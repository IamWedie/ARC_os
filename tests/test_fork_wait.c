/*
 * ARC OS fork/wait host test (task 2).
 *
 * Links against the REAL kernel/scheduler.c with tests/fork_wait_stub.c
 * supplying the hardware-facing pieces. This first slice verifies that
 * process_fork() copies the syscall trap frame into the timer-stub layout
 * that a context switch resumes through, and that child bookkeeping is
 * correct. Later tasks extend this same file with wait and dispatch tests.
 */
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern uint64_t fork_wait_stub_used_pages(void);
extern void fork_wait_stub_reset(void);
extern int fork_wait_stub_cr3_pid;
extern int fork_wait_stub_as_destroy_calls;

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

static void fake_entry(void) {}

static void reset_world(void) {
    fork_wait_stub_reset();
    scheduler_init();
    fork_wait_stub_cr3_pid = -1;
    fork_wait_stub_as_destroy_calls = 0;
}

/* Build a real user parent (PID 1) and make it the running process. */
static int make_user_parent(void) {
    void* kstack = pmm_alloc_pages(4);
    if (!kstack) return -1;

    int parent = process_create_user(USER_CODE_VA,
                                     (uint64_t)kstack + 4 * 4096ULL,
                                     "parent");
    if (parent < 0) return -1;
    scheduler_switch(0x1000);
    return parent;
}

/* A frame with a distinct known value in every field. */
static void fill_frame(syscall_frame_t* f) {
    f->r15      = 0xA000000000000001ULL;
    f->r14      = 0xA000000000000002ULL;
    f->r13      = 0xA000000000000003ULL;
    f->r12      = 0xA000000000000004ULL;
    f->rbp      = 0xA000000000000005ULL;
    f->rbx      = 0xA000000000000006ULL;
    f->r11      = 0xA000000000000007ULL;
    f->r10      = 0xA000000000000008ULL;
    f->r9       = 0xA000000000000009ULL;
    f->r8       = 0xA00000000000000AULL;
    f->rdi      = 0xA00000000000000BULL;
    f->rsi      = 0xA00000000000000CULL;
    f->rdx      = 0xA00000000000000DULL;
    f->rcx      = 0xA00000000000000EULL;
    f->rax      = 0xA00000000000000FULL;
    f->ret_slot = 0xA000000000000010ULL;
    f->rip      = 0x800000001234ULL;
    f->cs       = 0x23;
    f->rflags   = 0x246;
    f->user_rsp = 0x90000000ff00ULL;
    f->ss       = 0x1B;
}

static void test_fork_captures_trap_frame(void) {
    printf("\n=== Test 1: process_fork captures the syscall trap frame ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    CHECK(current_process == parent, "current = %d, want parent %d",
          current_process, parent);

    syscall_frame_t frame;
    fill_frame(&frame);

    /* The asm stub publishes the frame address through this global. */
    syscall_frame_rsp = (uint64_t)&frame;
    CHECK(syscall_frame_rsp == (uint64_t)&frame,
          "syscall_frame_rsp is not a writable global");

    uint64_t pages_before = fork_wait_stub_used_pages();
    int child = process_fork(parent, &frame);
    CHECK(child > 0, "process_fork returned %d", child);
    if (child <= 0) return;

    process_t* p = &processes[child];
    CHECK(p->ppid == parent, "child ppid = %d, want %d", p->ppid, parent);
    CHECK(p->user_mode == 1, "child user_mode = %d, want 1", p->user_mode);
    CHECK(p->state == PROC_READY, "child state = %d, want READY", p->state);
    CHECK(p->exit_code == 0, "child exit_code = %d", p->exit_code);
    CHECK(p->rip == frame.rip, "child rip = 0x%llx, want 0x%llx",
          (unsigned long long)p->rip, (unsigned long long)frame.rip);
    CHECK(p->level == 0, "child level = %d, want 0", p->level);
    CHECK(p->pml4_phys == 0, "child pml4_phys = 0x%llx, want 0",
          (unsigned long long)p->pml4_phys);
    CHECK(p->cpu_ticks == 0, "child cpu_ticks = %llu",
          (unsigned long long)p->cpu_ticks);
    CHECK(p->switches_in == 0, "child switches_in = %llu",
          (unsigned long long)p->switches_in);
    CHECK(p->slice_used == 0, "child slice_used = %u", p->slice_used);
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        CHECK(p->owned[i].base == 0 && p->owned[i].pages == 0,
              "child owned[%d] not cleared", i);
    }
    CHECK(strcmp(p->name, "forked") == 0, "child name = '%s', want 'forked'",
          p->name);

    CHECK(p->stack_base != 0, "child has no kernel stack");
    CHECK(p->stack_size == 4 * 4096ULL, "child stack_size = %llu",
          (unsigned long long)p->stack_size);
    CHECK(p->kstack_sp == p->stack_base + 4 * 4096ULL,
          "child kstack_sp = 0x%llx, want 0x%llx",
          (unsigned long long)p->kstack_sp,
          (unsigned long long)(p->stack_base + 4 * 4096ULL));
    CHECK(fork_wait_stub_used_pages() == pages_before + 4,
          "fork allocated %llu frames, want 4",
          (unsigned long long)(fork_wait_stub_used_pages() - pages_before));

    /*
     * 20 words at kstack_sp - 160: 15 GPRs (in PUSHREGS/POPREGS order)
     * then the 5 iretq words. No return slot; saved rax is forced to 0.
     */
    const uint64_t expected[20] = {
        frame.r15, frame.r14, frame.r13, frame.r12, frame.rbp,
        frame.rbx, frame.r11, frame.r10, frame.r9,  frame.r8,
        frame.rdi, frame.rsi, frame.rdx, frame.rcx, 0,
        frame.rip, frame.cs, frame.rflags, frame.user_rsp, frame.ss
    };
    uint64_t* gpr = (uint64_t*)(uintptr_t)(p->kstack_sp - 160);
    CHECK(p->rsp == (uint64_t)(uintptr_t)gpr,
          "child rsp = 0x%llx, want 0x%llx",
          (unsigned long long)p->rsp, (unsigned long long)(uintptr_t)gpr);

    for (int i = 0; i < 20; i++) {
        CHECK(gpr[i] == expected[i],
              "frame word %d = 0x%llx, want 0x%llx", i,
              (unsigned long long)gpr[i], (unsigned long long)expected[i]);
    }

    /* Same iretq words, expressed as negative offsets from the stack top. */
    uint64_t* fr = (uint64_t*)(uintptr_t)p->kstack_sp;
    CHECK(fr[-5] == frame.rip, "saved rip = 0x%llx, want 0x%llx",
          (unsigned long long)fr[-5], (unsigned long long)frame.rip);
    CHECK(fr[-4] == frame.cs, "saved cs = 0x%llx, want 0x%llx",
          (unsigned long long)fr[-4], (unsigned long long)frame.cs);
    CHECK(fr[-3] == frame.rflags, "saved rflags = 0x%llx, want 0x%llx",
          (unsigned long long)fr[-3], (unsigned long long)frame.rflags);
    CHECK(fr[-2] == frame.user_rsp, "saved user_rsp = 0x%llx, want 0x%llx",
          (unsigned long long)fr[-2], (unsigned long long)frame.user_rsp);
    CHECK(fr[-1] == frame.ss, "saved ss = 0x%llx, want 0x%llx",
          (unsigned long long)fr[-1], (unsigned long long)frame.ss);

    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after fork");
}

static void test_fork_rejects_invalid_inputs(void) {
    printf("\n=== Test 2: process_fork rejects invalid inputs ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);

    syscall_frame_t frame;
    fill_frame(&frame);
    uint64_t pages_before = fork_wait_stub_used_pages();

    CHECK(process_fork(0, &frame) == -1, "kernel/idle parent accepted");
    CHECK(process_fork(-1, &frame) == -1, "negative parent accepted");
    CHECK(process_fork(MAX_PROCESSES, &frame) == -1,
          "out-of-range parent accepted");
    CHECK(process_fork(parent, NULL) == -1, "NULL frame accepted");

    int kthread = process_create(fake_entry, "kthread");
    CHECK(kthread > 0, "kernel thread creation failed");
    CHECK(process_fork(kthread, &frame) == -1, "kernel-mode parent accepted");

    CHECK(fork_wait_stub_used_pages() == pages_before + 4,
          "rejected forks allocated frames");
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after rejected forks");
}

static void test_fork_out_of_slots(void) {
    printf("\n=== Test 3: process_fork fails cleanly with no free PID ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);

    /* Fill every remaining slot with kernel threads. */
    for (int i = 0; i < MAX_PROCESSES - 2; i++) {
        CHECK(process_create(fake_entry, "filler") > 0,
              "filler %d could not be created", i);
    }

    syscall_frame_t frame;
    fill_frame(&frame);
    uint64_t pages_before = fork_wait_stub_used_pages();

    CHECK(process_fork(parent, &frame) == -1,
          "fork succeeded with no free PID");
    CHECK(fork_wait_stub_used_pages() == pages_before,
          "failed fork leaked a kernel stack");
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after failed fork");
}

int main(void) {
    printf("ARC OS fork/wait test (real kernel/scheduler.c)\n");

    test_fork_captures_trap_frame();
    test_fork_rejects_invalid_inputs();
    test_fork_out_of_slots();

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
