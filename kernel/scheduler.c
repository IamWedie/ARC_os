/*
 * ARC OS - Preemptive MLFQ scheduler with explicit process states.
 *
 * Processes move through:
 *
 *   FREE -> READY -> RUNNING -> {READY, BLOCKED, ZOMBIE} -> FREE
 *
 * Four MLFQ levels (0 = highest priority), each an O(1) FIFO ring.  A timer
 * tick charges the running process; when its quantum expires it is demoted
 * one level.  Every SCHED_BOOST_TICKS ticks all ready processes return to
 * level 0 so long-running CPU hogs cannot starve interactive work.
 *
 * Process 0 is special: it is the kernel/idle stack and is never placed in a
 * run queue.  It runs only when no other process is ready, so a runnable
 * process never competes with the idle loop for CPU time.
 *
 * Context switching is a stack swap performed in the interrupt stubs:
 *
 *   irq0/apic_timer/yield stub -> handler(rsp) -> next process's saved rsp
 *
 * A return value of 0 means "stay on the current stack".  Unstarted
 * processes have a pre-built POPREGS+iretq frame, so the first switch enters
 * their entry point in ring 0 (kernel thread) or ring 3 (user process).
 */
#include "kernel.h"

#define PROCESS_STACK_PAGES 4
#define PUSHREG_COUNT 15         /* must match isr.S PUSHREGS (all GPRs) */
#define IRETQ_FRAME_BYTES 40     /* rip cs rflags rsp ss */

typedef struct {
    uint8_t pid[MAX_PROCESSES];
    uint8_t head;
    uint8_t count;
} run_queue_t;

process_t processes[MAX_PROCESSES];
volatile int current_process = 0;
volatile int num_processes = 0;
volatile int preempt_disabled = 0;

static run_queue_t runq[SCHED_MLFQ_LEVELS];
static uint8_t queued[MAX_PROCESSES];
static scheduler_stats_t stats;

void preempt_disable(void) { preempt_disabled++; }
void preempt_enable(void) {
    if (preempt_disabled > 0) preempt_disabled--;
}

/* ---------------------------------------------------------------- */
/*                    State and run-queue helpers                   */
/* ---------------------------------------------------------------- */

static void state_set(int pid, int next) {
    int prev = processes[pid].state;
    if (prev == next) return;

    switch (prev) {
    case PROC_READY:   stats.ready--;   break;
    case PROC_BLOCKED: stats.blocked--; break;
    case PROC_ZOMBIE:  stats.zombies--; break;
    case PROC_IDLE:    stats.idle--;    break;
    case PROC_FREE:    stats.active++;  break;
    default: break;                     /* RUNNING */
    }

    switch (next) {
    case PROC_READY:   stats.ready++;   break;
    case PROC_BLOCKED: stats.blocked++; break;
    case PROC_ZOMBIE:  stats.zombies++; break;
    case PROC_IDLE:    stats.idle++;    break;
    case PROC_FREE:    stats.active--;  break;
    default: break;                     /* RUNNING */
    }

    processes[pid].state = next;
}

static void queue_push(int pid, int level) {
    if (pid < 0 || pid >= MAX_PROCESSES) return;
    if (level < 0 || level >= SCHED_MLFQ_LEVELS)
        level = SCHED_MLFQ_LEVELS - 1;
    if (queued[pid]) return;

    run_queue_t* q = &runq[level];
    if (q->count >= MAX_PROCESSES) return;  /* cannot happen: MAX-1 ready */

    q->pid[(q->head + q->count) % MAX_PROCESSES] = (uint8_t)pid;
    q->count++;
    queued[pid] = 1;
}

static int queue_pop(int level) {
    run_queue_t* q = &runq[level];
    if (q->count == 0) return -1;

    int pid = q->pid[q->head];
    q->head = (uint8_t)((q->head + 1) % MAX_PROCESSES);
    q->count--;
    queued[pid] = 0;
    return pid;
}

/* Remove one process from whichever level holds it (block/kill path). */
static void queue_remove(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES || !queued[pid]) return;

    for (int l = 0; l < SCHED_MLFQ_LEVELS; l++) {
        run_queue_t* q = &runq[l];
        for (int i = 0; i < q->count; i++) {
            int idx = (q->head + i) % MAX_PROCESSES;
            if (q->pid[idx] != pid) continue;

            /* Shift the remainder left to preserve FIFO order. */
            for (int j = i; j + 1 < q->count; j++) {
                int a = (q->head + j) % MAX_PROCESSES;
                int b = (q->head + j + 1) % MAX_PROCESSES;
                q->pid[a] = q->pid[b];
            }
            q->count--;
            queued[pid] = 0;
            return;
        }
    }
}

static int pick_next(void) {
    for (int level = 0; level < SCHED_MLFQ_LEVELS; level++) {
        int pid = queue_pop(level);
        if (pid >= 0) return pid;
    }
    return -1;
}

static void set_name(process_t* p, const char* name) {
    int i = 0;
    while (name[i] && i < 31) {
        p->name[i] = name[i];
        i++;
    }
    p->name[i] = '\0';
}

/* ---------------------------------------------------------------- */
/*                        Public scheduler API                      */
/* ---------------------------------------------------------------- */

void scheduler_init(void) {
    stats = (scheduler_stats_t){0};

    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i] = (process_t){0};
        processes[i].pid = i;
        processes[i].state = PROC_FREE;
        queued[i] = 0;
    }
    for (int l = 0; l < SCHED_MLFQ_LEVELS; l++) {
        runq[l].head = 0;
        runq[l].count = 0;
    }

    current_process = 0;
    num_processes = 1;
    preempt_disabled = 0;

    /*
     * The boot path becomes process 0; it lives on the entry64 stack and
     * becomes the idle loop once other work exists.
     */
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    processes[0].rsp = rsp;
    processes[0].kstack_sp = (uint64_t)__stack_top;
    processes[0].user_mode = 0;
    processes[0].level = 0;
    state_set(0, PROC_RUNNING);
    set_name(&processes[0], "kernel");

    set_tss_rsp0(processes[0].kstack_sp);
}

static int process_build_common(uint64_t entry, const char* name,
                                int user_mode, uint64_t kstack_top,
                                uint64_t user_rsp) {
    int pid = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_FREE) {
            pid = i;
            break;
        }
    }
    if (pid < 0) return -1;

    /*
     * User processes already own a PROCESS_STACK_PAGES kernel stack (it is
     * the TSS RSP0 target), so build the initial iretq frame on it instead
     * of allocating a second one. Kernel threads get a fresh stack.
     */
    void* stack;
    uint64_t top;
    if (kstack_top) {
        top = kstack_top;
        stack = (void*)(top - PROCESS_STACK_PAGES * 4096);
    } else {
        stack = pmm_alloc_pages(PROCESS_STACK_PAGES);
        if (!stack) return -1;
        top = (uint64_t)stack + PROCESS_STACK_PAGES * 4096;
    }

    /* iretq frame at the top of the new (kernel) stack. */
    uint64_t* fr = (uint64_t*)top;
    fr[-5] = entry;
    fr[-4] = user_mode ? (GDTSEL_UC | 3) : 0x08;   /* CS (+ RPL 3 for user) */
    fr[-3] = 0x202;                          /* RFLAGS: IF set */
    fr[-2] = user_mode ? user_rsp : (top - 8);
    fr[-1] = user_mode ? (GDTSEL_UD | 3) : 0x10;   /* SS (+ RPL 3 for user) */

    /* Zeroed POPREGS block right below the frame. */
    uint64_t* gpr = (uint64_t*)(top - (PUSHREG_COUNT * 8) - IRETQ_FRAME_BYTES);
    for (int i = 0; i < PUSHREG_COUNT; i++) gpr[i] = 0;

    process_t* p = &processes[pid];
    p->rsp = (uint64_t)gpr;
    p->rip = entry;
    p->kstack_sp = kstack_top ? kstack_top : top;
    p->stack_base = (uint64_t)stack;
    p->stack_size = PROCESS_STACK_PAGES * 4096;
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        p->owned[i].base = 0;
        p->owned[i].pages = 0;
    }
    p->cpu_ticks = 0;
    p->switches_in = 0;
    p->user_mode = user_mode;
    p->level = 0;
    p->exit_code = 0;
    p->slice_used = 0;
    set_name(p, name);

    state_set(pid, PROC_READY);
    queue_push(pid, 0);

    if (pid + 1 > num_processes) num_processes = pid + 1;
    return pid;
}

int process_create(void (*entry)(void), const char* name) {
    return process_build_common((uint64_t)entry, name, 0, 0, 0);
}

int process_create_user(uint64_t entry, uint64_t kstack_top, const char* name) {
    return process_build_common(entry, name, 1, kstack_top, USER_STACK_TOP);
}

/*
 * Allocate a frame run and record it on the process in one step. A caller can
 * never forget to attach an allocation, and an attach failure never strands
 * frames: PMM allocation is the last operation that can fail, and any later
 * failure path is impossible once a slot and frames are both available.
 */
void* process_alloc_owned_frames(int pid, size_t pages) {
    if (pid < 1 || pid >= MAX_PROCESSES || pages == 0) return NULL;

    int state = processes[pid].state;
    if (state != PROC_READY && state != PROC_RUNNING &&
        state != PROC_BLOCKED && state != PROC_ZOMBIE)
        return NULL;

    int slot = -1;
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        if (processes[pid].owned[i].base == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL;

    void* frames = pmm_alloc_pages(pages);
    if (!frames) return NULL;

    processes[pid].owned[slot].base = (uint64_t)(uintptr_t)frames;
    processes[pid].owned[slot].pages = (uint64_t)pages;
    return frames;
}

/*
 * Shared scheduling core.  voluntary=0 is a timer preemption (charge the
 * quantum, maybe demote); voluntary=1 is a yield (no charge, same level).
 */
static uint64_t schedule(uint64_t rsp, int voluntary) {
    if (preempt_disabled) return 0;

    int prev = current_process;
    process_t* cur = &processes[prev];
    cur->rsp = rsp;

    if (voluntary) {
        stats.yields++;
        cur->slice_used = 0;
    } else {
        stats.ticks++;
        stats.preemptions++;
        cur->cpu_ticks++;

        if (cur->state == PROC_RUNNING) {
            cur->slice_used++;
            if (cur->slice_used >= SCHED_QUANTUM_TICKS) {
                cur->slice_used = 0;
                if (cur->level + 1 < SCHED_MLFQ_LEVELS) {
                    cur->level++;
                    stats.demotions++;
                }
            }
        }

        if (stats.ticks % SCHED_BOOST_TICKS == 0) scheduler_boost();
    }

    /*
     * Real runnable processes go back into their level's FIFO.  Idle is
     * parked instead: it is a fallback stack, never a queue entry, so it
     * cannot steal CPU from (or demote) an interactive process.
     */
    if (cur->state == PROC_RUNNING) {
        if (prev == 0) {
            state_set(0, PROC_IDLE);
        } else {
            state_set(prev, PROC_READY);
            queue_push(prev, cur->level);
        }
    }

    int next = pick_next();
    if (next < 0) {
        if (prev == 0) {
            /* Nothing to run: stay parked on the idle stack. */
            state_set(0, PROC_RUNNING);
            if (!voluntary) stats.idle_ticks++;
            return 0;
        }
        if (cur->state == PROC_READY) {
            /* We are the only runnable process: keep running. */
            queue_remove(prev);
            state_set(prev, PROC_RUNNING);
            return 0;
        }

        /* Current blocked or died; fall back to the kernel/idle stack. */
        state_set(0, PROC_RUNNING);
        processes[0].switches_in++;
        current_process = 0;
        set_tss_rsp0(processes[0].kstack_sp);
        stats.context_switches++;
        return processes[0].rsp;
    }

    state_set(next, PROC_RUNNING);
    if (next == prev) {
        /* The only runnable process is the one already on the CPU. */
        return 0;
    }

    current_process = next;
    processes[next].switches_in++;
    set_tss_rsp0(processes[next].kstack_sp);
    stats.context_switches++;
    return processes[next].rsp;
}

uint64_t scheduler_switch(uint64_t rsp) {
    return schedule(rsp, 0);
}

uint64_t scheduler_yield_handler(uint64_t rsp) {
    return schedule(rsp, 1);
}

void scheduler_yield(void) {
    __asm__ volatile("int $0x81" ::: "memory");
}

void scheduler_block_current(void) {
    if (current_process == 0) return;   /* idle must never block */
    if (scheduler_block(current_process) == 0) scheduler_yield();
}

void scheduler_exit_current(int code) {
    int pid = current_process;
    if (pid == 0) {
        for (;;) __asm__ volatile("cli; hlt");
    }

    processes[pid].exit_code = code;
    scheduler_kill(pid);

    /* The zombie is never re-queued, so this normally never returns. */
    for (;;) scheduler_yield();
}

int scheduler_block(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if (pid == 0) return -1;   /* idle must never block */

    int state = processes[pid].state;
    if (state != PROC_READY && state != PROC_RUNNING) return -1;

    if (state == PROC_READY) queue_remove(pid);
    state_set(pid, PROC_BLOCKED);
    return 0;
}

int scheduler_wake(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if (processes[pid].state != PROC_BLOCKED) return -1;

    processes[pid].level = 0;
    processes[pid].slice_used = 0;
    state_set(pid, PROC_READY);
    queue_push(pid, 0);
    return 0;
}

int scheduler_kill(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if (pid == 0) return -1;   /* idle stack cannot be killed */

    int state = processes[pid].state;
    if (state == PROC_FREE || state == PROC_ZOMBIE) return -1;

    if (state == PROC_READY) queue_remove(pid);
    state_set(pid, PROC_ZOMBIE);
    return 0;
}

int scheduler_reap(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if (pid == 0 || pid == current_process) return -1;
    if (processes[pid].state != PROC_ZOMBIE) return -1;

    if (processes[pid].stack_base && processes[pid].stack_size) {
        pmm_free_pages((void*)(uintptr_t)processes[pid].stack_base,
                       processes[pid].stack_size / 4096);
    }
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        proc_region_t* r = &processes[pid].owned[i];
        if (r->base && r->pages) {
            pmm_free_pages((void*)(uintptr_t)r->base, (size_t)r->pages);
        }
        r->base = 0;
        r->pages = 0;
    }

    processes[pid].rsp = 0;
    processes[pid].rip = 0;
    processes[pid].kstack_sp = 0;
    processes[pid].stack_base = 0;
    processes[pid].stack_size = 0;
    processes[pid].name[0] = '\0';
    state_set(pid, PROC_FREE);

    while (num_processes > 1 &&
           processes[num_processes - 1].state == PROC_FREE) {
        num_processes--;
    }
    return 0;
}

/*
 * Idle-loop reaper.  Process 0 owns no user resources, so it can safely free
 * every zombie's kernel stack and owned frame runs.  The scan is atomic with
 * respect to scheduling: a timer tick that arrives here sees preempt_disabled
 * and returns without touching the half-reaped process table.
 */
int scheduler_reap_zombies(void) {
    if (current_process != 0) return -1;

    int reaped = 0;
    preempt_disable();
    for (int pid = 1; pid < MAX_PROCESSES; pid++) {
        if (processes[pid].state == PROC_ZOMBIE &&
            scheduler_reap(pid) == 0) {
            reaped++;
        }
    }
    preempt_enable();
    return reaped;
}

void scheduler_boost(void) {
    /* Rebuild every queue so all ready work returns to the top level. */
    for (int l = 0; l < SCHED_MLFQ_LEVELS; l++) {
        runq[l].head = 0;
        runq[l].count = 0;
    }
    for (int i = 0; i < MAX_PROCESSES; i++) queued[i] = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_RUNNING ||
            processes[i].state == PROC_READY) {
            processes[i].level = 0;
            processes[i].slice_used = 0;
        }
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_READY) queue_push(i, 0);
    }

    stats.boosts++;
}

void scheduler_get_stats(scheduler_stats_t* out) {
    if (out) *out = stats;
}

int scheduler_state(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    return processes[pid].state;
}

/*
 * Internal consistency check used by host tests.  Call only when the
 * scheduler is quiescent (not between a state change and its switch).
 */
int scheduler_check_invariants(void) {
    unsigned ready = 0, blocked = 0, zombies = 0, idle_count = 0;
    unsigned active = 0, running = 0;
    uint8_t seen[MAX_PROCESSES] = {0};

    for (int l = 0; l < SCHED_MLFQ_LEVELS; l++) {
        run_queue_t* q = &runq[l];
        if (q->count > MAX_PROCESSES) return -10;
        if (q->head >= MAX_PROCESSES) return -11;

        for (int i = 0; i < q->count; i++) {
            int pid = q->pid[(q->head + i) % MAX_PROCESSES];
            if (pid < 0 || pid >= MAX_PROCESSES) return -12;
            if (pid == 0) return -16;   /* idle must never be queued */
            if (seen[pid]) return -13;
            seen[pid] = 1;
            if (processes[pid].state != PROC_READY) return -14;
            if (processes[pid].level != l) return -15;
        }
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        switch (processes[i].state) {
        case PROC_READY:
            ready++;
            active++;
            if (!seen[i]) return -20;
            break;
        case PROC_RUNNING:
            running++;
            active++;
            if (seen[i]) return -21;
            break;
        case PROC_IDLE:
            idle_count++;
            active++;
            if (i != 0) return -26;     /* only process 0 may park */
            if (seen[i]) return -27;
            break;
        case PROC_BLOCKED:
            blocked++;
            active++;
            if (seen[i]) return -22;
            break;
        case PROC_ZOMBIE:
            zombies++;
            active++;
            if (seen[i]) return -23;
            break;
        case PROC_FREE:
            if (seen[i]) return -24;
            break;
        default:
            return -25;
        }
    }

    if (ready != stats.ready) return -30;
    if (blocked != stats.blocked) return -31;
    if (zombies != stats.zombies) return -32;
    if (active != stats.active) return -33;
    if (running != 1) return -34;
    if (current_process < 0 || current_process >= MAX_PROCESSES) return -35;
    if (processes[current_process].state != PROC_RUNNING) return -36;
    if (idle_count != stats.idle) return -37;

    return 0;
}
