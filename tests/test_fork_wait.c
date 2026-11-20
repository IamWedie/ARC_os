/*
 * ARC OS fork/wait host test.
 *
 * Links against the REAL kernel/scheduler.c with tests/fork_wait_stub.c
 * supplying the hardware-facing pieces. Verifies that process_fork() copies
 * the syscall trap frame into the timer-stub layout that a context switch
 * resumes through, that parent-aware exit/reaping keeps a parent's zombie for
 * wait(), and that scheduler_wait_child() implements the collect/block/ECHILD
 * policy. Later tasks extend this same file with syscall dispatch tests.
 */
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern uint64_t fork_wait_stub_used_pages(void);
extern void fork_wait_stub_reset(void);
extern int fork_wait_stub_cr3_pid;
extern unsigned fork_wait_stub_add_ref_calls(void);
extern unsigned fork_wait_stub_release_calls(void);
extern int fork_wait_stub_total_refs(void);

/* Page-table leaves carry flags in their low bits. */
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

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

/* Physical page backing `va` in `pid`'s private root, or NULL when unmapped.
 * Host tests use identity-mapped PMM frames, so the leaf's PFN is a real
 * pointer we can inspect and mutate. */
static uint8_t* user_page_phys(int pid, uint64_t va) {
    uint64_t* leaf = pt_lookup((uint64_t*)(uintptr_t)as_root(pid), va);
    if (!leaf) return NULL;
    return (uint8_t*)(uintptr_t)(*leaf & PT_ADDR_MASK);
}

static void reset_world(void) {
    fork_wait_stub_reset();
    scheduler_init();
    fork_wait_stub_cr3_pid = -1;
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

/* Build a user parent with one forked child; returns the child pid. */
static int make_parent_with_child(int* parent_out) {
    int parent = make_user_parent();
    if (parent <= 0) return -1;

    syscall_frame_t frame;
    fill_frame(&frame);
    int child = process_fork(parent, &frame);
    if (child <= 0) return -1;

    *parent_out = parent;
    return child;
}

static void test_parented_zombie_survives_idle_reaper(void) {
    printf("\n=== Test 4: idle reaper skips parented zombies ===\n");
    reset_world();

    int parent = -1;
    int child = make_parent_with_child(&parent);
    CHECK(child > 0, "child fork failed (pid %d)", child);
    if (child <= 0) return;

    CHECK(scheduler_kill(child) == 0, "kill child failed");
    CHECK(processes[child].state == PROC_ZOMBIE, "child not zombie");

    /* The idle loop owns the reaper; it must not steal a parent's zombie. */
    current_process = 0;
    int reaped = scheduler_reap_zombies();
    current_process = parent;

    CHECK(processes[child].state == PROC_ZOMBIE,
          "idle reaper stole parented zombie pid %d", child);
    CHECK(reaped == 0, "idle reaper reaped %d processes, want 0", reaped);
}

static void test_exit_reparents_children(void) {
    printf("\n=== Test 5: process exit reparents children to PID 0 ===\n");
    reset_world();

    int parent = -1;
    int child = make_parent_with_child(&parent);
    CHECK(child > 0, "child fork failed (pid %d)", child);
    if (child <= 0) return;
    CHECK(processes[child].ppid == parent, "precondition: ppid = %d",
          processes[child].ppid);

    /* Exercise the exit bookkeeping directly: the public entry point is
     * noreturn (it loops on int $0x81), which a host test cannot execute. */
    scheduler_exit_process(parent, 42);

    CHECK(processes[parent].state == PROC_ZOMBIE,
          "exiting process state = %d, want ZOMBIE", processes[parent].state);
    CHECK(processes[parent].exit_code == 42,
          "exit_code = %d, want 42", processes[parent].exit_code);
    CHECK(processes[child].ppid == 0,
          "orphaned child ppid = %d, want 0", processes[child].ppid);
}

static void test_child_exit_wakes_blocked_parent(void) {
    printf("\n=== Test 6: child exit wakes a blocked parent ===\n");
    reset_world();

    int parent = -1;
    int child = make_parent_with_child(&parent);
    CHECK(child > 0, "child fork failed (pid %d)", child);
    if (child <= 0) return;

    CHECK(scheduler_block(parent) == 0, "block parent failed");
    CHECK(processes[parent].state == PROC_BLOCKED, "parent not blocked");

    scheduler_exit_process(child, 7);

    CHECK(processes[parent].state == PROC_READY,
          "parent state = %d, want READY", processes[parent].state);
    CHECK(processes[child].ppid == parent,
          "child ppid = %d, want %d", processes[child].ppid, parent);
    CHECK(processes[child].exit_code == 7,
          "child exit_code = %d, want 7", processes[child].exit_code);
}

static void test_reap_clears_ppid(void) {
    printf("\n=== Test 7: scheduler_reap clears ppid ===\n");
    reset_world();

    int parent = -1;
    int child = make_parent_with_child(&parent);
    CHECK(child > 0, "child fork failed (pid %d)", child);
    if (child <= 0) return;

    CHECK(scheduler_kill(child) == 0, "kill child failed");
    CHECK(processes[child].ppid == parent, "precondition: ppid = %d",
          processes[child].ppid);
    CHECK(scheduler_reap(child) == 0, "reap failed");

    CHECK(processes[child].state == PROC_FREE,
          "reaped state = %d, want FREE", processes[child].state);
    CHECK(processes[child].ppid == 0,
          "reaped ppid = %d, want 0", processes[child].ppid);
}

static void test_wait_child_policy(void) {
    printf("\n=== Test 8: scheduler_wait_child collects/blocks/ECHILD ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    if (parent <= 0) return;

    int status = -99;
    CHECK(scheduler_wait_child(parent, &status) == -1,
          "no children must return ECHILD");
    CHECK(status == -99, "status written with no children");

    syscall_frame_t frame;
    fill_frame(&frame);
    int child = process_fork(parent, &frame);
    CHECK(child > 0, "child fork failed (pid %d)", child);
    if (child <= 0) return;

    /* A live child means "block", not "no children". */
    CHECK(scheduler_wait_child(parent, &status) == 0,
          "live child must return 0 (block)");
    CHECK(status == -99, "status written for a live child");

    /* Its zombie is collected with status, reaped and given back to the PMM. */
    uint64_t pages_before = fork_wait_stub_used_pages();
    processes[child].exit_code = 55;
    CHECK(scheduler_kill(child) == 0, "kill child failed");
    int got = scheduler_wait_child(parent, &status);
    CHECK(got == child, "wait returned %d, want child %d", got, child);
    CHECK(status == 55, "status = %d, want 55", status);
    CHECK(processes[child].state == PROC_FREE, "collected child not reaped");
    CHECK(processes[child].ppid == 0, "collected child ppid not cleared");
    CHECK(fork_wait_stub_used_pages() == pages_before - 4,
          "collecting a child freed %llu frames, want 4",
          (unsigned long long)(pages_before - fork_wait_stub_used_pages()));

    /* No children left after collection. */
    CHECK(scheduler_wait_child(parent, &status) == -1,
          "ECHILD after all children reaped");

    /* A NULL status pointer is allowed. */
    int child2 = process_fork(parent, &frame);
    CHECK(child2 > 0, "second child fork failed (pid %d)", child2);
    if (child2 <= 0) return;
    processes[child2].exit_code = 9;
    CHECK(scheduler_kill(child2) == 0, "kill second child failed");
    CHECK(scheduler_wait_child(parent, NULL) == child2,
          "NULL status pointer was rejected");
}

static void test_sys_fork_clones_as_and_fds(void) {
    printf("\n=== Test 9: sys_fork clones the address space and fd table ===\n");
    reset_world();

    /* A kernel/idle caller has no user frame to clone. */
    int saved = current_process;
    current_process = 0;
    CHECK(sys_fork() == -1, "kernel/idle parent forked");
    current_process = saved;

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    if (parent <= 0) return;
    CHECK(as_create(parent) == 0, "as_create(parent) failed");

    uint8_t* src = (uint8_t*)pmm_alloc_pages(2);
    CHECK(src != NULL, "source page allocation failed");
    if (!src) return;
    src[0] = 0x11;
    src[1] = 0x22;
    src[4096] = 0x33;
    src[4097] = 0x44;
    CHECK(as_map_user(parent, USER_CODE_VA, (uint64_t)(uintptr_t)src, 2,
                      PT_PG_RW) == 0, "source mapping failed");

    int filefd = fd_open(parent, "fork.txt", O_CREAT | O_RDWR);
    CHECK(filefd >= 3, "fd_open returned %d", filefd);
    if (filefd < 0) return;
    CHECK(fork_wait_stub_add_ref_calls() == 1,
          "fd_open must acquire exactly one slot ref");
    CHECK(fork_wait_stub_total_refs() == 1, "slot refcount != 1 after open");

    syscall_frame_t frame;
    fill_frame(&frame);
    syscall_frame_rsp = (uint64_t)&frame;

    uint64_t pages_before = fork_wait_stub_used_pages();
    int child = sys_fork();
    CHECK(child > 0, "sys_fork returned %d", child);
    if (child <= 0) return;
    CHECK(child == parent + 1, "child pid = %d, want %d", child, parent + 1);
    CHECK(processes[child].ppid == parent, "child ppid = %d, want %d",
          processes[child].ppid, parent);
    CHECK(preempt_disabled == 0, "preempt left disabled after sys_fork");
    CHECK(fork_wait_stub_used_pages() > pages_before, "fork allocated nothing");

    CHECK(processes[child].pml4_phys != 0 &&
          processes[child].pml4_phys != processes[parent].pml4_phys,
          "child root missing or shared with parent");

    for (int fd = FD_STDIN; fd <= FD_STDERR; fd++) {
        CHECK(processes[child].fds[fd].kind == processes[parent].fds[fd].kind,
              "child fd %d kind = %d, want %d", fd,
              processes[child].fds[fd].kind, processes[parent].fds[fd].kind);
    }
    CHECK(processes[child].fds[filefd].kind == FD_ENTRY_FILE,
          "cloned file fd is not a file");
    CHECK(processes[child].fds[filefd].ofd ==
          processes[parent].fds[filefd].ofd, "cloned fd points at a new OFD");
    CHECK(fork_wait_stub_add_ref_calls() == 1,
          "fd_clone_process took a second filesystem slot ref");
    CHECK(fork_wait_stub_total_refs() == 1,
          "fd_clone_process changed the slot refcount");

    /* Deep copy: same contents, different frames, independent writes. */
    uint8_t* dst0 = user_page_phys(child, USER_CODE_VA);
    uint8_t* dst1 = user_page_phys(child, USER_CODE_VA + 4096);
    CHECK(dst0 != NULL && dst1 != NULL, "child user leaves missing");
    CHECK(dst0 != src && dst1 != src + 4096, "child reused parent frames");
    if (dst0 && dst1) {
        CHECK(dst0[0] == 0x11 && dst0[1] == 0x22 &&
              dst1[0] == 0x33 && dst1[1] == 0x44,
              "cloned page contents differ from parent");
        src[0] = 0x99;
        CHECK(dst0[0] == 0x11, "parent write changed the child page");
        dst0[1] = 0x77;
        CHECK(src[1] == 0x22, "child write changed the parent page");
    }

    /* A cloned fd shares its OFD (and thus its offset); the slot reference is
     * still held once. Closing one descriptor must not release the slot. */
    CHECK(fd_write(child, filefd, "ab", 2) == 2, "write via cloned fd failed");
    CHECK(fd_seek(parent, filefd, 0, SEEK_SET) == 0, "parent seek failed");
    char buf[4] = {0, 0, 0, 0};
    CHECK(fd_read(parent, filefd, buf, 2) == 2 &&
          buf[0] == 'a' && buf[1] == 'b',
          "parent did not see bytes written by the child");
    CHECK(fd_close(child, filefd) == 0, "close of cloned fd failed");
    CHECK(fork_wait_stub_release_calls() == 0,
          "closing one of two cloned fds released the shared slot");
    CHECK(fd_size(parent, filefd) == 2, "parent fd broke when child closed");
    CHECK(fd_close(parent, filefd) == 0, "close of parent fd failed");
    CHECK(fork_wait_stub_release_calls() == 1,
          "slot ref not released when the last fd closed");
    CHECK(fork_wait_stub_total_refs() == 0, "slot refs leaked after closes");

    CHECK(scheduler_kill(child) == 0, "kill child failed");
    CHECK(scheduler_reap(child) == 0, "reap child failed");
    as_destroy(parent);
    pmm_free_pages(src, 2);
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after sys_fork");
}

static void test_sys_wait_collect_and_echild(void) {
    printf("\n=== Test 10: sys_wait collects a zombie or reports ECHILD ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    if (parent <= 0) return;
    CHECK(as_create(parent) == 0, "as_create(parent) failed");

    int status = -99;
    CHECK(sys_wait(&status) == -1, "ECHILD with no children");
    CHECK(status == -99, "status written with no children");

    syscall_frame_t frame;
    fill_frame(&frame);
    syscall_frame_rsp = (uint64_t)&frame;

    int child = sys_fork();
    CHECK(child > 0, "sys_fork returned %d", child);
    if (child <= 0) return;

    processes[child].exit_code = 55;
    CHECK(scheduler_kill(child) == 0, "kill child failed");
    uint64_t pages_before = fork_wait_stub_used_pages();

    status = -99;
    int waited = sys_wait(&status);
    CHECK(waited == child, "sys_wait returned %d, want %d", waited, child);
    CHECK(status == 55, "status = %d, want 55", status);
    CHECK(processes[child].state == PROC_FREE, "collected child not reaped");
    CHECK(processes[child].ppid == 0, "collected child ppid not cleared");
    CHECK(processes[child].pml4_phys == 0, "collected child root not freed");
    CHECK(fork_wait_stub_used_pages() < pages_before,
          "collecting a child freed no frames");

    CHECK(sys_wait(&status) == -1, "ECHILD after all children collected");
    CHECK(status == 55, "ECHILD changed the status output");
    CHECK(sys_wait(NULL) == -1, "NULL status with no children");

    /* NULL status is allowed when there is a real zombie to collect. */
    int child2 = sys_fork();
    CHECK(child2 > 0, "second sys_fork returned %d", child2);
    if (child2 <= 0) return;
    processes[child2].exit_code = 9;
    CHECK(scheduler_kill(child2) == 0, "kill second child failed");
    CHECK(sys_wait(NULL) == child2, "sys_wait(NULL) rejected a zombie child");

    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after sys_wait");
}

static void test_syscall_dispatch_fork_wait(void) {
    printf("\n=== Test 11: SYS_FORK / SYS_WAIT dispatch ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    if (parent <= 0) return;
    CHECK(as_create(parent) == 0, "as_create(parent) failed");

    syscall_frame_t frame;
    fill_frame(&frame);
    syscall_frame_rsp = (uint64_t)&frame;

    int child = (int)syscall_dispatch(SYS_FORK, 0, 0, 0);
    CHECK(child > 0, "SYS_FORK dispatch returned %d", child);
    if (child <= 0) return;

    processes[child].exit_code = 7;
    CHECK(scheduler_kill(child) == 0, "kill child failed");
    int status = -1;
    int got = (int)syscall_dispatch(SYS_WAIT,
                                    (uint64_t)(uintptr_t)&status, 0, 0);
    CHECK(got == child, "SYS_WAIT dispatch returned %d, want %d", got, child);
    CHECK(status == 7, "status = %d, want 7", status);
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after dispatch");
}

static void test_sys_fork_failure_paths(void) {
    printf("\n=== Test 12: sys_fork failure paths leave no residue ===\n");
    reset_world();

    int parent = make_user_parent();
    CHECK(parent > 0, "user parent creation failed (pid %d)", parent);
    if (parent <= 0) return;
    CHECK(as_create(parent) == 0, "as_create(parent) failed");

    syscall_frame_t frame;
    fill_frame(&frame);
    syscall_frame_rsp = (uint64_t)&frame;

    /* Corrupt one parent fd so fd_clone_process() must fail after the child
     * slot and address space have already been built. */
    processes[parent].fds[FD_MAX - 1].kind = FD_ENTRY_FILE;
    processes[parent].fds[FD_MAX - 1].ofd = -1;
    uint64_t pages_before = fork_wait_stub_used_pages();
    int child = sys_fork();
    CHECK(child == -1, "fork with a broken fd returned %d", child);
    CHECK(preempt_disabled == 0, "preempt left disabled after clone failure");
    CHECK(fork_wait_stub_used_pages() == pages_before,
          "failed fork leaked child resources");
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (i == parent) continue;
        CHECK(processes[i].ppid != parent,
              "slot %d is still linked to the parent after clone failure", i);
    }
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after clone failure");

    /* No slot left: process_fork fails before any clone is attempted. */
    for (int i = 0; i < MAX_PROCESSES - 2; i++) {
        CHECK(process_create(fake_entry, "filler") > 0,
              "filler %d could not be created", i);
    }
    pages_before = fork_wait_stub_used_pages();
    CHECK(sys_fork() == -1, "sys_fork succeeded with no free PID");
    CHECK(preempt_disabled == 0, "preempt left disabled after PID failure");
    CHECK(fork_wait_stub_used_pages() == pages_before,
          "failed fork allocated frames with no free PID");
    CHECK(scheduler_check_invariants() == 0,
          "run-queue invariants violated after failed fork");
}

int main(void) {
    printf("ARC OS fork/wait test (real kernel/scheduler.c)\n");

    test_fork_captures_trap_frame();
    test_fork_rejects_invalid_inputs();
    test_fork_out_of_slots();
    test_parented_zombie_survives_idle_reaper();
    test_exit_reparents_children();
    test_child_exit_wakes_blocked_parent();
    test_reap_clears_ppid();
    test_wait_child_policy();
    test_sys_fork_clones_as_and_fds();
    test_sys_wait_collect_and_echild();
    test_syscall_dispatch_fork_wait();
    test_sys_fork_failure_paths();

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
