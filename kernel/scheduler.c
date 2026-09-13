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
#define PUSHREG_COUNT 9          /* must match isr.S PUSHREGS */
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

    /* The boot path becomes process 0; it lives on the entry64 stack. */
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    processes[0].rsp = rsp;
    processes[0].running = 1;
    processes[0].started = 1;
    processes[0].rip = 0;
    int i = 0;
    const char* nm = "kernel";
    while (nm[i] && i < 31) { processes[0].name[i] = nm[i]; i++; }
    current_process = 0;
    num_processes = 1;
}

void process_create(void (*entry)(void), const char* name) {
    if (num_processes >= MAX_PROCESSES) return;

    void* stack = pmm_alloc_pages(PROCESS_STACK_PAGES);
    if (!stack) return;
    uint64_t top = (uint64_t)stack + PROCESS_STACK_PAGES * 4096;

    /* iretq frame at the top of the new stack. */
    uint64_t* fr = (uint64_t*)top;
    fr[-5] = (uint64_t)entry;
    fr[-4] = 0x08;              /* CS: ring 0 code */
    fr[-3] = 0x202;             /* RFLAGS: IF set */
    fr[-2] = top - 8;           /* initial RSP (16-aligned stack, ABI entry) */
    fr[-1] = 0x10;              /* SS: data segment */

    /* zeroed POPREGS block right below the frame. */
    uint64_t* gpr = (uint64_t*)(top - (PUSHREG_COUNT * 8) - IRETQ_FRAME_BYTES);
    for (int i = 0; i < PUSHREG_COUNT; i++) gpr[i] = 0;

    process_t* p = &processes[num_processes];
    p->rsp = (uint64_t)gpr;
    p->rip = (uint64_t)entry;
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
    if (next == prev) return 0;
    processes[next].started = 1;
    return processes[next].rsp;
}

int scheduler_stats(int pid, uint64_t* ticks) {
    (void)pid;
    (void)ticks;
    return 0;
}