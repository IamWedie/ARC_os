/*
 * ARC OS - Preemptive scheduler.
 *
 * Round-robin across kernel threads. Process 0 is always the boot/kernel
 * context. Each preemption is a stack swap done in irq0_stub:
 *
 *   irq0_handler(rsp) -> scheduler_switch(rsp) returns the next process's
 *   saved stack pointer (or 0 if nothing changed), and the stub loads it
 *   before POPREGS + iretq.
 *
 * An unstarted process is given a pre-built stack whose POPREGS block is
 * zeroed and whose iretq frame enters its entry function in ring 0, so the
 * first switch into it starts it cleanly.
 */
#include "kernel.h"

#define PROCESS_STACK_PAGES 4
#define PUSHREG_COUNT 15         /* must match isr.S PUSHREGS (all GPRs) */
#define IRETQ_FRAME_BYTES 40     /* rip cs rflags rsp ss */

process_t processes[MAX_PROCESSES];
volatile int current_process = 0;
volatile int num_processes = 0;
volatile int preempt_disabled = 0;

void preempt_disable(void) { preempt_disabled++; }
void preempt_enable(void) {
    if (preempt_disabled > 0) preempt_disabled--;
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pid = i;
        processes[i].running = 0;
        processes[i].started = 0;
        processes[i].rsp = 0;
        processes[i].rip = 0;
        processes[i].stack_base = 0;
        processes[i].stack_size = 0;
        processes[i].name[0] = '\0';
    }

    /*
     * The boot path becomes process 0; it lives on the entry64 stack.
     * Its kernel-stack top is used for TSS RSP0 whenever it cannot be
     * switched out of Ring 3.
     */
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    processes[0].rsp = rsp;
    processes[0].kstack_sp = (uint64_t)__stack_top;
    processes[0].user_mode = 0;
    processes[0].running = 1;
    processes[0].started = 1;
    processes[0].rip = 0;
    int i = 0;
    const char* nm = "kernel";
    while (nm[i] && i < 31) { processes[0].name[i] = nm[i]; i++; }
    current_process = 0;
    num_processes = 1;
}

static int process_build_common(uint64_t entry, const char* name,
                                int user_mode, uint64_t kstack_top,
                                uint64_t user_rsp) {
    if (num_processes >= MAX_PROCESSES) return -1;

    void* stack = pmm_alloc_pages(PROCESS_STACK_PAGES);
    if (!stack) return -1;
    uint64_t top = (uint64_t)stack + PROCESS_STACK_PAGES * 4096;

    /* iretq frame at the top of the new (kernel) stack. */
    uint64_t* fr = (uint64_t*)top;
    fr[-5] = entry;
    fr[-4] = user_mode ? (GDTSEL_UC | 3) : 0x08;   /* CS (+ RPL 3 for user) */
    fr[-3] = 0x202;                          /* RFLAGS: IF set */
    fr[-2] = user_mode ? user_rsp : (top - 8);
    fr[-1] = user_mode ? (GDTSEL_UD | 3) : 0x10;   /* SS (+ RPL 3 for user) */

    /* zeroed POPREGS block right below the frame. */
    uint64_t* gpr = (uint64_t*)(top - (PUSHREG_COUNT * 8) - IRETQ_FRAME_BYTES);
    for (int i = 0; i < PUSHREG_COUNT; i++) gpr[i] = 0;

    process_t* p = &processes[num_processes];
    p->rsp = (uint64_t)gpr;
    p->rip = entry;
    p->kstack_sp = kstack_top;
    p->user_mode = user_mode;
    p->running = 1;
    p->started = 0;
    p->stack_base = (uint64_t)stack;
    p->stack_size = PROCESS_STACK_PAGES * 4096;
    int j = 0;
    while (name[j] && j < 31) {
        p->name[j] = name[j];
        j++;
    }
    p->name[j] = '\0';
    num_processes++;
    return p->pid;
}

void process_create(void (*entry)(void), const char* name) {
    process_build_common((uint64_t)entry, name, 0, 0, 0);
}

int process_create_user(uint64_t entry, uint64_t kstack_top, const char* name) {
    return process_build_common(entry, name, 1, kstack_top, USER_STACK_TOP);
}

uint64_t scheduler_switch(uint64_t rsp) {
    if (preempt_disabled) return 0;
    if (num_processes < 1) return 0;

    int prev = current_process;
    processes[prev].rsp = rsp;

    int next = prev;
    for (int i = 1; i <= num_processes; i++) {
        next = (next + 1) % num_processes;
        if (processes[next].running) break;
    }

    current_process = next;
    if (next == prev) {
        /* Stay on the same process: keep the kernel stack selection valid. */
        return 0;
    }
    /* Privileged entry from Ring 3 uses the new process's kernel stack. */
    set_tss_rsp0(processes[next].kstack_sp);
    processes[next].started = 1;
    return processes[next].rsp;
}

int scheduler_stats(int pid, uint64_t* ticks) {
    (void)pid;
    (void)ticks;
    return 0;
}