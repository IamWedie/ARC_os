/*
 * ARC OS Scheduler Test
 *
 * Links against the REAL kernel/scheduler.c with host stubs for the TSS and
 * the boot stack, and tests/pmm_stub.c providing physical frames.
 *
 * The tests drive the scheduler through its public API and verify the run
 * queue, MLFQ levels, state counters and invariants after every operation.
 */
#include <stdint.h>
#include <stdio.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

extern void pmm_stub_reset(void);
extern uint64_t pmm_stub_used_pages(void);

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
    pmm_stub_reset();
    scheduler_init();
}

static uint64_t rsp_of(int pid) {
    return processes[pid].rsp;
}

/* Verify every process state matches the accounting counters. */
static void check_totals(const char* where) {
    unsigned ready = 0, blocked = 0, zombies = 0, idle = 0, active = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        switch (processes[i].state) {
        case PROC_READY:   ready++;   active++; break;
        case PROC_RUNNING:            active++; break;
        case PROC_IDLE:    idle++;    active++; break;
        case PROC_BLOCKED: blocked++; active++; break;
        case PROC_ZOMBIE:  zombies++; active++; break;
        case PROC_FREE: break;
        default:
            CHECK(0, "%s: pid %d has invalid state %d", where, i,
                  processes[i].state);
            break;
        }
    }

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.ready == ready, "%s: ready counter %u != scan %u",
          where, s.ready, ready);
    CHECK(s.blocked == blocked, "%s: blocked counter %u != scan %u",
          where, s.blocked, blocked);
    CHECK(s.zombies == zombies, "%s: zombie counter %u != scan %u",
          where, s.zombies, zombies);
    CHECK(s.idle == idle, "%s: idle counter %u != scan %u",
          where, s.idle, idle);
    CHECK(s.active == active, "%s: active counter %u != scan %u",
          where, s.active, active);
    CHECK(scheduler_check_invariants() == 0,
          "%s: run-queue invariants violated", where);
}

static void test_initial_state(void) {
    printf("\n=== Test 1: initial state ===\n");
    reset_world();

    CHECK(current_process == 0, "current_process = %d", current_process);
    CHECK(num_processes == 1, "num_processes = %d", num_processes);
    CHECK(processes[0].state == PROC_RUNNING,
          "pid 0 state = %d", processes[0].state);
    CHECK(processes[0].level == 0, "pid 0 level = %d", processes[0].level);
    CHECK(scheduler_state(0) == PROC_RUNNING, "scheduler_state(0) wrong");
    CHECK(scheduler_state(1) == PROC_FREE, "pid 1 should be FREE");
    CHECK(scheduler_state(-1) == -1, "negative pid must be rejected");
    CHECK(scheduler_state(MAX_PROCESSES) == -1,
          "out-of-range pid must be rejected");
    CHECK(scheduler_block(0) == -1, "block(idle) accepted");
    CHECK(scheduler_kill(0) == -1, "kill(idle) accepted");
    CHECK(scheduler_reap(0) == -1, "reap(idle) accepted");

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.ticks == 0, "ticks = %llu", (unsigned long long)s.ticks);
    CHECK(s.context_switches == 0, "switches = %llu",
          (unsigned long long)s.context_switches);
    CHECK(s.active == 1, "active = %u", s.active);
    CHECK(s.ready == 0, "ready = %u", s.ready);
    CHECK(s.idle == 0, "idle = %u", s.idle);
    check_totals("init");
}

static void test_creation_and_fifo(void) {
    printf("\n=== Test 2: creation and FIFO round-robin ===\n");
    reset_world();

    int p1 = process_create(fake_entry, "one");
    int p2 = process_create(fake_entry, "two");
    CHECK(p1 == 1, "first pid = %d", p1);
    CHECK(p2 == 2, "second pid = %d", p2);
    CHECK(processes[p1].state == PROC_READY, "pid 1 not READY");
    CHECK(processes[p2].state == PROC_READY, "pid 2 not READY");
    CHECK(processes[p1].level == 0, "pid 1 not at level 0");
    CHECK(processes[p1].stack_base != 0, "pid 1 has no stack");
    check_totals("after create");

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.ready == 2, "ready = %u", s.ready);
    CHECK(s.active == 3, "active = %u", s.active);

    uint64_t p1_rsp = rsp_of(p1);
    uint64_t next = scheduler_switch(0x1111);
    CHECK(next == p1_rsp, "first switch returned 0x%llx, want 0x%llx",
          (unsigned long long)next, (unsigned long long)p1_rsp);
    CHECK(current_process == p1, "current = %d, want %d",
          current_process, p1);
    CHECK(processes[0].state == PROC_IDLE, "pid 0 state = %d, want IDLE",
          processes[0].state);
    CHECK(processes[0].rsp == 0x1111, "pid 0 rsp not saved");
    CHECK(processes[p1].state == PROC_RUNNING, "pid 1 not RUNNING");
    check_totals("after switch to p1");

    scheduler_get_stats(&s);
    CHECK(s.ticks == 1, "ticks = %llu", (unsigned long long)s.ticks);
    CHECK(s.preemptions == 1, "preemptions = %llu",
          (unsigned long long)s.preemptions);
    CHECK(s.context_switches == 1, "switches = %llu",
          (unsigned long long)s.context_switches);

    /* FIFO within level 0: p1 -> p2 -> p1. Idle never runs while work is
     * ready, because it is not a queue entry at all. */
    uint64_t p2_rsp = rsp_of(p2);
    next = scheduler_switch(0x2222);
    CHECK(next == p2_rsp, "second switch should pick pid 2");
    CHECK(current_process == p2, "current = %d, want %d",
          current_process, p2);
    CHECK(processes[p1].state == PROC_READY, "pid 1 not re-queued");
    CHECK(rsp_of(p1) == 0x2222, "pid 1 rsp not saved");

    next = scheduler_switch(0x3333);
    CHECK(current_process == p1, "third switch should pick pid 1, got %d",
          current_process);
    CHECK(next == 0x2222, "pid 1 resumed rsp = 0x%llx",
          (unsigned long long)next);
    CHECK(processes[0].state == PROC_IDLE,
          "idle was scheduled while work was ready");

    next = scheduler_switch(0x4444);
    CHECK(current_process == p2, "fourth switch should pick pid 2");
    CHECK(next == 0x3333, "pid 2 resumed rsp = 0x%llx",
          (unsigned long long)next);
    check_totals("after FIFO cycle");
}

static void test_idle_fallback(void) {
    printf("\n=== Test 2b: idle is a fallback, not a peer ===\n");
    reset_world();

    int p1 = process_create(fake_entry, "one");
    int p2 = process_create(fake_entry, "two");

    scheduler_switch(0x1000);
    CHECK(current_process == p1, "current = %d", current_process);
    CHECK(scheduler_state(0) == PROC_IDLE, "idle state = %d",
          scheduler_state(0));

    /* While real work is ready, the idle process must never be selected. */
    for (int i = 0; i < 2; i++) {
        scheduler_switch(0x2000 + (uint64_t)i);
        CHECK(current_process != 0, "idle selected while work was ready");
    }
    CHECK(current_process == p1, "current = %d", current_process);
    CHECK(rsp_of(p1) == 0x2000, "p1 rsp = 0x%llx",
          (unsigned long long)rsp_of(p1));

    /* Park all real work; the scheduler must fall back to the idle stack. */
    CHECK(scheduler_block(p1) == 0, "block(p1) failed");
    CHECK(scheduler_block(p2) == 0, "block(p2) failed");

    uint64_t r = scheduler_switch(0x3000);
    CHECK(current_process == 0, "did not fall back to idle");
    CHECK(r == 0x1000, "idle resume rsp = 0x%llx",
          (unsigned long long)r);
    CHECK(processes[0].state == PROC_RUNNING, "idle not RUNNING");
    check_totals("after idle fallback");

    /* A woken process preempts idle on the next scheduling pass. */
    CHECK(scheduler_wake(p1) == 0, "wake(p1) failed");
    r = scheduler_switch(0x4000);
    CHECK(r == 0x3000, "p1 resume rsp = 0x%llx",
          (unsigned long long)r);
    CHECK(current_process == p1, "woken process did not preempt idle");
    check_totals("after wake from idle");
}

static void test_quantum_and_priority(void) {
    printf("\n=== Test 3: quantum demotion and priorities ===\n");
    reset_world();

    int p1 = process_create(fake_entry, "one");

    uint64_t first = scheduler_switch(0xA000);
    CHECK(first == rsp_of(p1), "did not switch to the only ready process");
    CHECK(current_process == p1, "current = %d", current_process);

    /* With only p1 ready, every extra switch stays on p1. */
    for (int i = 1; i <= SCHED_QUANTUM_TICKS; i++) {
        uint64_t next = scheduler_switch(0xB000 + (uint64_t)i);
        CHECK(next == 0, "tick %d switched unexpectedly", i);
        CHECK(current_process == p1, "tick %d left pid 1", i);
    }

    CHECK(processes[p1].level == 1, "pid 1 level = %d, want 1",
          processes[p1].level);
    CHECK(processes[p1].slice_used == 0, "slice = %u, want 0",
          processes[p1].slice_used);
    CHECK(processes[p1].cpu_ticks == SCHED_QUANTUM_TICKS,
          "cpu_ticks = %llu", (unsigned long long)processes[p1].cpu_ticks);

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.demotions == 1, "demotions = %llu",
          (unsigned long long)s.demotions);
    CHECK(s.context_switches == 1, "switches = %llu",
          (unsigned long long)s.context_switches);

    /* A level-0 process must preempt a level-1 process. */
    int p2 = process_create(fake_entry, "two");
    CHECK(p2 == 2, "p2 pid = %d", p2);

    uint64_t next = scheduler_switch(0xC000);
    CHECK(next == rsp_of(p2), "level-0 process did not preempt level-1 work");
    CHECK(current_process == p2, "current = %d, want %d",
          current_process, p2);
    CHECK(processes[p1].level == 1, "pid 1 level changed to %d",
          processes[p1].level);
    check_totals("after priority switch");

    /* The demoted process must still be ready, not lost. */
    CHECK(processes[p1].state == PROC_READY, "pid 1 was lost");
    CHECK(processes[p2].state == PROC_RUNNING, "pid 2 not running");
}

static void test_block_wake(void) {
    printf("\n=== Test 4: blocking and waking ===\n");
    reset_world();

    int p1 = process_create(fake_entry, "one");
    scheduler_switch(0x1000);
    CHECK(current_process == p1, "current = %d", current_process);

    CHECK(scheduler_block(p1) == 0, "block(running) failed");
    CHECK(processes[p1].state == PROC_BLOCKED, "pid 1 state = %d",
          processes[p1].state);

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.blocked == 1, "blocked = %u", s.blocked);
    CHECK(s.ready == 0, "ready = %u", s.ready);
    CHECK(s.idle == 1, "idle = %u", s.idle);

    uint64_t next = scheduler_switch(0x2000);
    CHECK(next == 0x1000, "idle resume rsp = 0x%llx",
          (unsigned long long)next);
    CHECK(current_process == 0, "current = %d", current_process);
    CHECK(processes[0].state == PROC_RUNNING, "idle not RUNNING");
    check_totals("after block and switch");

    CHECK(scheduler_wake(p1) == 0, "wake failed");
    CHECK(processes[p1].state == PROC_READY, "pid 1 not READY after wake");
    CHECK(processes[p1].level == 0, "woken process not at level 0");

    next = scheduler_switch(0x3000);
    CHECK(next == 0x2000, "pid 1 resume rsp = 0x%llx",
          (unsigned long long)next);
    CHECK(current_process == p1, "current = %d", current_process);
    check_totals("after wake");

    CHECK(scheduler_block(MAX_PROCESSES) == -1, "block(bad pid) accepted");
    CHECK(scheduler_wake(MAX_PROCESSES) == -1, "wake(bad pid) accepted");
    CHECK(scheduler_block(0) == -1, "block(idle) accepted");
    CHECK(scheduler_wake(p1) == -1, "wake(RUNNING) accepted");
    CHECK(scheduler_wake(0) == -1, "wake(RUNNING idle) accepted");
}

static void test_yield(void) {
    printf("\n=== Test 5: voluntary yield ===\n");
    reset_world();
    int p1 = process_create(fake_entry, "one");
    int p2 = process_create(fake_entry, "two");
    uint64_t p2_rsp = rsp_of(p2);

    scheduler_switch(0x1000);
    CHECK(current_process == p1, "current = %d", current_process);

    uint64_t r = scheduler_yield_handler(0x5555);
    CHECK(r == p2_rsp, "yield did not switch to pid 2 (0x%llx)",
          (unsigned long long)r);
    CHECK(current_process == p2, "current = %d", current_process);
    CHECK(processes[p1].state == PROC_READY, "pid 1 not READY");
    CHECK(processes[p1].rsp == 0x5555, "pid 1 rsp = 0x%llx",
          (unsigned long long)processes[p1].rsp);

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.yields == 1, "yields = %llu", (unsigned long long)s.yields);
    CHECK(s.context_switches == 2, "switches = %llu",
          (unsigned long long)s.context_switches);
    check_totals("after first yield");

    r = scheduler_yield_handler(0x6666);
    CHECK(r == 0x5555, "second yield resume rsp = 0x%llx",
          (unsigned long long)r);
    CHECK(current_process == p1, "current = %d", current_process);
    check_totals("after second yield");

    /* Yielding with no other ready process must not count a switch. */
    reset_world();
    r = scheduler_yield_handler(0x7777);
    CHECK(r == 0, "solo yield returned 0x%llx", (unsigned long long)r);
    CHECK(current_process == 0, "current = %d", current_process);
    CHECK(processes[0].state == PROC_RUNNING, "idle did not stay running");
    scheduler_get_stats(&s);
    CHECK(s.yields == 1, "solo yields = %llu",
          (unsigned long long)s.yields);
    CHECK(s.context_switches == 0, "solo switches = %llu",
          (unsigned long long)s.context_switches);
    CHECK(s.idle_ticks == 0, "solo idle_ticks = %llu",
          (unsigned long long)s.idle_ticks);
    check_totals("after solo yield");
}

static void test_kill_reap(void) {
    printf("\n=== Test 6: zombie and reap ===\n");
    reset_world();
    uint64_t pages_before = pmm_stub_used_pages();

    int p1 = process_create(fake_entry, "one");
    CHECK(pmm_stub_used_pages() == pages_before + 4,
          "process creation did not allocate 4 frames");

    scheduler_switch(0x1000);
    CHECK(current_process == p1, "current = %d", current_process);

    CHECK(scheduler_kill(p1) == 0, "kill failed");
    CHECK(processes[p1].state == PROC_ZOMBIE, "pid 1 state = %d",
          processes[p1].state);
    CHECK(scheduler_kill(p1) == -1, "double kill accepted");

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.zombies == 1, "zombies = %u", s.zombies);

    uint64_t next = scheduler_switch(0x2000);
    CHECK(next == 0x1000, "zombie was resumed");
    CHECK(current_process == 0, "did not fall back to idle");
    check_totals("after kill");

    CHECK(scheduler_reap(p1) == 0, "reap failed");
    CHECK(processes[p1].state == PROC_FREE, "pid 1 not FREE");
    CHECK(pmm_stub_used_pages() == pages_before,
          "reap leaked frames: %llu used",
          (unsigned long long)pmm_stub_used_pages());
    CHECK(scheduler_reap(p1) == -1, "double reap accepted");
    CHECK(scheduler_reap(0) == -1, "reaped a live process");

    /* The freed slot must be reused by the next creation. */
    int p2 = process_create(fake_entry, "two");
    CHECK(p2 == p1, "slot %d was not reused (got %d)", p1, p2);
    check_totals("after reap and reuse");
}

/*
 * The loader allocates two user frame runs outside the kernel stack. Until
 * they are recorded on the process, reap cannot free them. This test drives
 * the same shape and checks that frame usage returns to baseline.
 */
static void test_user_owned_frames(void) {
    printf("\n=== Test 6b: user-owned frames are freed on reap ===\n");
    reset_world();
    uint64_t before = pmm_stub_used_pages();

    void* kstack = pmm_alloc_pages(4);
    CHECK(kstack != NULL, "kernel stack allocation failed");
    int pid = process_create_user(USER_CODE_VA,
                                  (uint64_t)kstack + 4 * 4096ULL, "shell");
    CHECK(pid >= 0, "process_create_user failed");
    CHECK(pmm_stub_used_pages() == before + 4,
          "creation allocated %llu frames, want 4",
          (unsigned long long)(pmm_stub_used_pages() - before));

    uint8_t* code = (uint8_t*)process_alloc_owned_frames(pid, 3);
    uint8_t* ustack =
        (uint8_t*)process_alloc_owned_frames(pid, USER_STACK_PAGES);
    CHECK(code != NULL, "code run could not be attached");
    CHECK(ustack != NULL, "user stack run could not be attached");
    CHECK(pmm_stub_used_pages() == before + 4 + 3 + USER_STACK_PAGES,
          "attach allocated %llu frames, want %u",
          (unsigned long long)(pmm_stub_used_pages() - before),
          4 + 3 + USER_STACK_PAGES);

    CHECK(processes[pid].owned[0].base == (uint64_t)code,
          "code run base not recorded");
    CHECK(processes[pid].owned[0].pages == 3, "code run size not recorded");
    CHECK(processes[pid].owned[1].base == (uint64_t)ustack,
          "stack run base not recorded");
    CHECK(processes[pid].owned[1].pages == USER_STACK_PAGES,
          "stack run size not recorded");

    scheduler_kill(pid);
    CHECK(scheduler_reap(pid) == 0, "reap of owned process failed");
    CHECK(pmm_stub_used_pages() == before,
          "reap leaked owned frames: %llu still used",
          (unsigned long long)(pmm_stub_used_pages() - before));
    CHECK(processes[pid].owned[0].base == 0, "code slot not cleared");
    CHECK(processes[pid].owned[1].base == 0, "stack slot not cleared");
    check_totals("after owned-frame reap");

    /* Repeated spawns must not ratchet the frame count upward. */
    for (int i = 0; i < 8; i++) {
        void* ks = pmm_alloc_pages(4);
        int p = process_create_user(USER_CODE_VA,
                                    (uint64_t)ks + 4 * 4096ULL, "shell");
        void* c = process_alloc_owned_frames(p, 3);
        void* s = process_alloc_owned_frames(p, USER_STACK_PAGES);
        CHECK(c != NULL && s != NULL, "cycle %d attach failed", i);
        scheduler_kill(p);
        CHECK(scheduler_reap(p) == 0, "cycle %d reap failed", i);
        CHECK(pmm_stub_used_pages() == before,
              "cycle %d leaked %llu frames", i,
              (unsigned long long)(pmm_stub_used_pages() - before));
    }
    check_totals("after repeated owned-frame cycles");
}

static void test_owned_frame_validation(void) {
    printf("\n=== Test 6c: owned-frame API validation ===\n");
    reset_world();
    uint64_t before = pmm_stub_used_pages();

    CHECK(process_alloc_owned_frames(0, 1) == NULL, "pid 0 accepted");
    CHECK(process_alloc_owned_frames(-1, 1) == NULL, "negative pid accepted");
    CHECK(process_alloc_owned_frames(MAX_PROCESSES, 1) == NULL,
          "out-of-range pid accepted");
    CHECK(pmm_stub_used_pages() == before, "invalid attach allocated frames");

    int p = process_create(fake_entry, "owner");
    CHECK(p > 0, "process creation failed");
    CHECK(process_alloc_owned_frames(p, 0) == NULL, "zero-page attach accepted");
    CHECK(pmm_stub_used_pages() == before + 4,
          "zero-page attach allocated frames");

    /* Four slots exactly; a fifth attach must fail without taking a frame. */
    CHECK(process_alloc_owned_frames(p, 1) != NULL, "slot 1 failed");
    CHECK(process_alloc_owned_frames(p, 1) != NULL, "slot 2 failed");
    CHECK(process_alloc_owned_frames(p, 1) != NULL, "slot 3 failed");
    CHECK(process_alloc_owned_frames(p, 1) != NULL, "slot 4 failed");
    CHECK(process_alloc_owned_frames(p, 1) == NULL, "fifth attach accepted");
    CHECK(pmm_stub_used_pages() == before + 8,
          "capacity check leaked frames");

    scheduler_kill(p);
    CHECK(scheduler_reap(p) == 0, "reap failed");
    CHECK(pmm_stub_used_pages() == before, "reap did not free all owned runs");

    /* A reaped (FREE) slot cannot own frames. */
    CHECK(process_alloc_owned_frames(p, 1) == NULL, "FREE slot accepted");
    CHECK(pmm_stub_used_pages() == before, "attach to FREE slot allocated");
}

static void test_boost(void) {
    printf("\n=== Test 7: priority boost ===\n");
    reset_world();
    int p1 = process_create(fake_entry, "one");
    scheduler_switch(0xA000);

    for (int i = 0; i < SCHED_QUANTUM_TICKS; i++)
        scheduler_switch(0xB000 + (uint64_t)i);
    CHECK(processes[p1].level == 1, "setup did not demote to level 1");

    scheduler_boost();
    CHECK(processes[p1].level == 0, "boost left level %d",
          processes[p1].level);
    CHECK(processes[p1].slice_used == 0, "boost left slice %u",
          processes[p1].slice_used);

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.boosts == 1, "boosts = %llu", (unsigned long long)s.boosts);
    check_totals("after boost");

    /* Boosting the running process resets its level but must not requeue it. */
    CHECK(processes[p1].state == PROC_RUNNING, "boost changed state to %d",
          processes[p1].state);
    uint64_t next = scheduler_switch(0xC000);
    CHECK(next == 0, "boosted process did not run");
    CHECK(current_process == p1, "current = %d", current_process);
}

static void test_preempt_disable(void) {
    printf("\n=== Test 8: preemption disable ===\n");
    reset_world();
    int p1 = process_create(fake_entry, "one");

    preempt_disable();
    CHECK(scheduler_switch(0x1111) == 0, "switch while disabled");
    CHECK(current_process == 0, "current = %d", current_process);
    CHECK(processes[0].rsp != 0x1111, "disabled switch saved rsp");

    scheduler_stats_t s;
    scheduler_get_stats(&s);
    CHECK(s.ticks == 0, "disabled switch counted a tick");
    check_totals("while disabled");

    preempt_enable();
    uint64_t next = scheduler_switch(0x2222);
    CHECK(next == rsp_of(p1), "switch after enable failed");
    CHECK(current_process == p1, "current = %d", current_process);
    check_totals("after enable");
}

int main(void) {
    printf("ARC OS scheduler test (real kernel/scheduler.c)\n");

    test_initial_state();
    test_creation_and_fifo();
    test_idle_fallback();
    test_quantum_and_priority();
    test_block_wake();
    test_yield();
    test_kill_reap();
    test_user_owned_frames();
    test_owned_frame_validation();
    test_boost();
    test_preempt_disable();

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
