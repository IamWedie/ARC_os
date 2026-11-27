/*
 * ARC OS user-exception isolation host test.
 *
 * Links the REAL kernel/scheduler.c and the new exception policy with a small
 * hardware stub. It proves that synthetic ring-3 #DE/#UD/#PF frames terminate
 * only the faulting process, hand the CPU to another process, preserve the
 * shared trap-frame layout, and leave cleanup to exactly one reap. Ring-0
 * frames must stay fatal and must not trigger user termination.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern uint64_t exception_stub_used_pages(void);
extern void exception_stub_reset(void);
extern int exception_stub_as_destroy_calls;

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
    exception_stub_reset();
    scheduler_init();
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

static void fill_syscall_frame(syscall_frame_t* f) {
    uint64_t* words = (uint64_t*)f;
    for (size_t i = 0; i < sizeof(*f) / sizeof(*words); i++)
        words[i] = 0x1000 + i;
    f->cs = 0x23;
    f->rflags = 0x246;
    f->user_rsp = USER_STACK_TOP - 128;
    f->ss = 0x1B;
}

static void fill_exception_frame(exception_frame_t* f, uint64_t vec,
                                 uint64_t err, int user) {
    uint64_t* words = (uint64_t*)f;
    for (size_t i = 0; i < sizeof(*f) / sizeof(*words); i++)
        words[i] = 0x2000 + i;

    f->r15 = 0xA1; f->r14 = 0xA2; f->r13 = 0xA3; f->r12 = 0xA4;
    f->rbp = 0xA5; f->rbx = 0xA6; f->r11 = 0xA7; f->r10 = 0xA8;
    f->r9  = 0xA9; f->r8  = 0xAA; f->rdi = 0xAB; f->rsi = 0xAC;
    f->rdx = 0xAD; f->rcx = 0xAE; f->rax = 0xAF;
    f->vec = vec;
    f->err = err;
    f->rip = USER_CODE_VA + 0x40;
    f->cs = user ? 0x23 : 0x08;
    f->rflags = 0x246;
    f->rsp = USER_STACK_TOP - 128;
    f->ss = user ? 0x1B : 0x10;
}

/* Ring-0 interrupt frames have 15 GPRs then RIP/CS/RFLAGS; iretq with CS.RPL
 * 0 stops there. This is the layout schedule() saves for PID 0/yield entry. */
static void make_ring0_restore_frame(uint64_t* words, uint64_t rip) {
    for (size_t i = 0; i < 18; i++) words[i] = 0x3000 + i;
    words[15] = rip;     /* offset 120: RIP */
    words[16] = 0x08;    /* offset 128: kernel CS */
    words[17] = 0x202;   /* offset 136: RFLAGS with IF */
}

static void switch_to_pid(int want) {
    for (int i = 0; i < MAX_PROCESSES * 2 && current_process != want; i++)
        scheduler_switch(0x1000);
}

/* Parent PID 1, sibling PID 2, faulting child PID 3 (child is current). */
static int make_faulting_child(int* parent_out, int* sibling_out) {
    int parent = make_user_parent();
    if (parent <= 0) return -1;

    void* sibling_stack = pmm_alloc_pages(4);
    if (!sibling_stack) return -1;
    int sibling = process_create_user(USER_CODE_VA,
                                      (uint64_t)sibling_stack + 4 * 4096ULL,
                                      "sibling");
    if (sibling <= 0) return -1;

    syscall_frame_t frame;
    fill_syscall_frame(&frame);
    int child = process_fork(parent, &frame);
    if (child <= 0) return -1;

    switch_to_pid(child);
    if (current_process != child) return -1;

    *parent_out = parent;
    *sibling_out = sibling;
    return child;
}

static void test_frame_layout(void) {
    printf("\n=== Test 1: exception frame preserves the stub ABI ===\n");

    CHECK(sizeof(exception_frame_t) == 176,
          "frame size = %u, want 176", (unsigned)sizeof(exception_frame_t));
    CHECK(offsetof(exception_frame_t, vec) == 120,
          "vec offset = %u, want 120", (unsigned)offsetof(exception_frame_t, vec));
    CHECK(offsetof(exception_frame_t, err) == 128,
          "err offset = %u, want 128", (unsigned)offsetof(exception_frame_t, err));
    CHECK(offsetof(exception_frame_t, rip) == 136,
          "rip offset = %u, want 136", (unsigned)offsetof(exception_frame_t, rip));
    CHECK(offsetof(exception_frame_t, ss) == 168,
          "ss offset = %u, want 168", (unsigned)offsetof(exception_frame_t, ss));
}

static void test_process_restore_frames(void) {
    printf("\n=== Test 2: scheduler frames match POPREGS/iretq ABI ===\n");
    reset_world();

    void* kstack = pmm_alloc_pages(4);
    CHECK(kstack != NULL, "could not allocate user kernel stack");
    if (!kstack) return;
    uint64_t top = (uint64_t)kstack + 4 * 4096ULL;
    int pid = process_create_user(USER_CODE_VA, top, "abi-user");
    CHECK(pid > 0, "user frame setup failed (pid %d)", pid);
    if (pid <= 0) return;

    uint64_t* f = (uint64_t*)processes[pid].rsp;
    CHECK((uint64_t)f == top - 160ULL,
          "initial user rsp = 0x%llx, want top-160",
          (unsigned long long)(uint64_t)f);
    int gprs_zero = 1;
    for (int i = 0; i < 15; i++) if (f[i] != 0) gprs_zero = 0;
    CHECK(gprs_zero, "initial user GPR block is not zeroed");
    CHECK(f[15] == USER_CODE_VA, "user RIP slot = 0x%llx",
          (unsigned long long)f[15]);
    CHECK(f[16] == 0x23, "user CS slot = 0x%llx, want 0x23",
          (unsigned long long)f[16]);
    CHECK(f[17] == 0x202, "user RFLAGS slot = 0x%llx, want 0x202",
          (unsigned long long)f[17]);
    CHECK(f[18] == USER_STACK_TOP, "user RSP slot = 0x%llx",
          (unsigned long long)f[18]);
    CHECK(f[19] == 0x1B, "user SS slot = 0x%llx, want 0x1B",
          (unsigned long long)f[19]);

    reset_world();
    int kid = process_create((void (*)(void))(uintptr_t)0x1234, "abi-kernel");
    CHECK(kid > 0, "kernel frame setup failed (pid %d)", kid);
    if (kid > 0) {
        uint64_t* kf = (uint64_t*)processes[kid].rsp;
        CHECK(kf[15] == 0x1234, "kernel RIP slot = 0x%llx",
              (unsigned long long)kf[15]);
        CHECK(kf[16] == 0x08, "kernel CS slot = 0x%llx, want 0x08",
              (unsigned long long)kf[16]);
        CHECK(kf[17] == 0x202, "kernel RFLAGS slot = 0x%llx, want 0x202",
              (unsigned long long)kf[17]);
    }

    reset_world();
    int parent = make_user_parent();
    CHECK(parent > 0, "fork frame parent setup failed (pid %d)", parent);
    if (parent > 0) {
        syscall_frame_t sf;
        fill_syscall_frame(&sf);
        int child = process_fork(parent, &sf);
        CHECK(child > 0, "fork frame setup failed (pid %d)", child);
        if (child > 0) {
            uint64_t* ff = (uint64_t*)processes[child].rsp;
            CHECK(ff[0] == sf.r15 && ff[5] == sf.rbx && ff[10] == sf.rdi,
                  "forked GPR block does not match saved syscall frame");
            CHECK(ff[14] == 0, "forked child rax = %llu, want 0",
                  (unsigned long long)ff[14]);
            CHECK(ff[15] == sf.rip && ff[16] == sf.cs &&
                  ff[17] == sf.rflags && ff[18] == sf.user_rsp &&
                  ff[19] == sf.ss,
                  "forked iretq words do not match saved syscall frame");
        }
    }
}

static void test_saved_timer_frame(void) {
    printf("\n=== Test 3: scheduler saves the timer/IRQ restore frame ===\n");
    reset_world();

    int pid = make_user_parent();
    CHECK(pid > 0, "timer-frame setup failed (pid %d)", pid);
    if (pid <= 0) return;

    uint64_t saved[20];
    for (size_t i = 0; i < 20; i++) saved[i] = 0x4000 + i;
    saved[15] = USER_CODE_VA + 0x80;
    saved[16] = 0x23;
    saved[17] = 0x246;
    saved[18] = USER_STACK_TOP - 256;
    saved[19] = 0x1B;

    uint64_t ret = scheduler_switch((uint64_t)saved);
    CHECK(ret == 0, "only-runnable process switched away (ret 0x%llx)",
          (unsigned long long)ret);
    CHECK(processes[pid].rsp == (uint64_t)saved,
          "scheduler did not save the stub frame pointer");
    uint64_t* out = (uint64_t*)processes[pid].rsp;
    int same = 1;
    for (size_t i = 0; i < 20; i++) if (out[i] != saved[i]) same = 0;
    CHECK(same, "saved timer frame words changed");
}

static void test_origin_and_exit_codes(void) {
    printf("\n=== Test 4: CS RPL decides origin and fault exit code ===\n");
    reset_world();

    exception_frame_t f;
    fill_exception_frame(&f, 0, 0, 1);
    CHECK(exception_frame_is_user(&f) == 1, "ring-3 frame not classified user");
    CHECK(exception_frame_is_fatal(&f) == 0, "ring-3 frame classified fatal");
    CHECK(exception_exit_code(0) == 128, "#DE exit code = %d, want 128",
          exception_exit_code(0));
    CHECK(exception_exit_code(6) == 134, "#UD exit code = %d, want 134",
          exception_exit_code(6));
    CHECK(exception_exit_code(14) == 142, "#PF exit code = %d, want 142",
          exception_exit_code(14));

    fill_exception_frame(&f, 14, 6, 0);
    CHECK(exception_frame_is_user(&f) == 0, "ring-0 frame classified user");
    CHECK(exception_frame_is_fatal(&f) == 1, "ring-0 frame not classified fatal");

    /* A scheduled user process must not make a kernel-mode fault look user. */
    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    fill_exception_frame(&f, 14, 0, 0);
    CHECK(exception_user_fault_exit(&f, (uint64_t)&f) == 0,
          "kernel fault entered user termination path");
    CHECK(processes[parent].state == PROC_RUNNING,
          "kernel fault changed running process state to %d",
          processes[parent].state);
}

static void test_user_fault(uint64_t vec, uint64_t err, int expected_code) {
    printf("\n=== User fault vector %llu: isolate, switch, reap once ===\n",
           (unsigned long long)vec);
    reset_world();

    int parent = -1;
    int sibling = -1;
    int child = make_faulting_child(&parent, &sibling);
    CHECK(child > 0, "faulting child setup failed (pid %d)", child);
    if (child <= 0) return;

    CHECK(process_alloc_owned_frames(child, 2) != NULL,
          "could not give child owned frames to reclaim");
    uint64_t pages_before_reap = exception_stub_used_pages();

    exception_frame_t f;
    fill_exception_frame(&f, vec, err, 1);
    uint64_t fault_rsp = (uint64_t)&f;

    uint64_t next_rsp = exception_user_fault_exit(&f, fault_rsp);

    CHECK(processes[child].state == PROC_ZOMBIE,
          "faulting child state = %d, want ZOMBIE", processes[child].state);
    CHECK(processes[child].exit_code == expected_code,
          "faulting child exit_code = %d, want %d",
          processes[child].exit_code, expected_code);
    CHECK(current_process != child, "scheduler kept the faulting process");
    CHECK(current_process > 0, "scheduler did not switch to a live process");
    CHECK(processes[current_process].state == PROC_RUNNING,
          "survivor state = %d, want RUNNING", processes[current_process].state);
    CHECK(next_rsp != 0, "scheduler returned no stack pointer");
    CHECK(next_rsp != fault_rsp, "scheduler resumed the faulting frame");
    CHECK(next_rsp == processes[current_process].rsp,
          "returned rsp does not match the survivor's saved frame");
    CHECK(processes[parent].state == PROC_RUNNING ||
          processes[parent].state == PROC_READY,
          "parent process was not preserved (state %d)", processes[parent].state);
    CHECK(processes[sibling].state == PROC_RUNNING ||
          processes[sibling].state == PROC_READY,
          "sibling process was not preserved (state %d)", processes[sibling].state);
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after user fault");

    int status = -1;
    CHECK(scheduler_wait_child(parent, &status) == child,
          "parent wait did not collect the faulting child");
    CHECK(status == expected_code, "wait status = %d, want %d",
          status, expected_code);
    CHECK(processes[child].state == PROC_FREE,
          "faulting child not reaped (state %d)", processes[child].state);
    CHECK(exception_stub_as_destroy_calls == 1,
          "as_destroy called %d times, want 1", exception_stub_as_destroy_calls);
    CHECK(exception_stub_used_pages() == pages_before_reap - 6,
          "reap freed %llu frames, want 6",
          (unsigned long long)(pages_before_reap - exception_stub_used_pages()));

    status = -1;
    CHECK(scheduler_wait_child(parent, &status) == -1,
          "second wait unexpectedly collected another child");
    CHECK(exception_stub_as_destroy_calls == 1,
          "second wait reaped resources twice");
}

static void test_pid0_no_survivor_fallback(void) {
    printf("\n=== Test 5: no-survivor fault switches to PID 0 frame ===\n");
    reset_world();

    /* Boot makes PID 0 save a real yield frame before any ring-3 code runs. */
    int fault_pid = make_user_parent();
    CHECK(fault_pid > 0, "no-survivor setup failed (pid %d)", fault_pid);
    if (fault_pid <= 0) return;

    uint64_t idle_frame[18];
    const uint64_t idle_rip = 0x200000ULL;
    make_ring0_restore_frame(idle_frame, idle_rip);
    processes[0].rsp = (uint64_t)idle_frame;

    exception_frame_t f;
    fill_exception_frame(&f, 14, 6, 1);   /* real hardware-error-code vector */
    uint64_t fault_rsp = (uint64_t)&f;

    uint64_t next_rsp = exception_user_fault_exit(&f, fault_rsp);

    CHECK(processes[fault_pid].state == PROC_ZOMBIE,
          "faulting process state = %d, want ZOMBIE",
          processes[fault_pid].state);
    CHECK(processes[fault_pid].exit_code == 142,
          "faulting process exit_code = %d, want 142",
          processes[fault_pid].exit_code);
    CHECK(current_process == 0, "no-survivor current pid = %d, want 0",
          current_process);
    CHECK(next_rsp != 0 && next_rsp != fault_rsp,
          "faulting frame was returned as the replacement stack");
    CHECK(next_rsp == (uint64_t)idle_frame,
          "scheduler returned 0x%llx, want PID 0 frame 0x%llx",
          (unsigned long long)next_rsp,
          (unsigned long long)(uint64_t)idle_frame);
    CHECK(processes[0].state == PROC_RUNNING,
          "PID 0 state = %d, want RUNNING", processes[0].state);

    uint64_t* idle = (uint64_t*)next_rsp;
    CHECK(idle[15] == idle_rip && idle[15] != f.rip,
          "PID 0 RIP slot does not point at the idle loop");
    CHECK(idle[16] == 0x08, "PID 0 CS slot = 0x%llx, want ring 0",
          (unsigned long long)idle[16]);
    CHECK(idle[17] == 0x202, "PID 0 RFLAGS slot = 0x%llx, want 0x202",
          (unsigned long long)idle[17]);
    CHECK(exception_stub_as_destroy_calls == 0,
          "address space destroyed before the faulting process was reaped");

    CHECK(scheduler_reap_zombies() == 1,
          "idle reaper did not collect the orphaned faulting process");
    CHECK(processes[fault_pid].state == PROC_FREE,
          "faulting process not reaped (state %d)",
          processes[fault_pid].state);
    CHECK(exception_stub_as_destroy_calls == 1,
          "as_destroy called %d times, want 1",
          exception_stub_as_destroy_calls);
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after PID 0 fallback");
}

int main(void) {
    printf("ARC OS user exception isolation host test\n");

    test_frame_layout();
    test_process_restore_frames();
    test_saved_timer_frame();
    test_origin_and_exit_codes();
    test_user_fault(0, 0, 128);   /* #DE: no error code */
    test_user_fault(6, 0, 134);   /* #UD: no error code */
    test_user_fault(14, 6, 142);  /* #PF: CPU error code */
    test_pid0_no_survivor_fallback();

    printf("\n%d checks passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
