/*
 * ARC OS - fork/wait syscall bodies.
 *
 * syscall_stub() publishes the address of its saved GPR block in
 * syscall_frame_rsp before calling syscall_dispatch(), so the frame is still
 * live when sys_fork() runs. process_fork() copies that frame onto a fresh
 * kernel stack; this module completes the clone by duplicating the private
 * user address space and the descriptor table. All three steps happen with
 * preemption disabled: a timer tick must never observe a half-built child.
 */
#include "kernel.h"

long sys_fork(void) {
    int parent_pid = current_process;
    if (parent_pid <= 0 || parent_pid >= MAX_PROCESSES) return -1;
    if (!processes[parent_pid].user_mode) return -1;

    const syscall_frame_t* frame =
        (const syscall_frame_t*)(uintptr_t)syscall_frame_rsp;

    preempt_disable();

    int child = process_fork(parent_pid, frame);
    if (child < 0) {
        preempt_enable();
        return -1;
    }

    if (as_clone(parent_pid, child) != 0 ||
        fd_clone_process(&processes[parent_pid], &processes[child]) != 0) {
        /*
         * The child may already own a private root, copied user frames and
         * cloned OFD references. kill + reap releases all of them before the
         * slot is offered to another process.
         */
        scheduler_kill(child);
        scheduler_reap(child);
        child = -1;
    }

    preempt_enable();
    return child;
}

long sys_wait(int* status) {
    for (;;) {
        int pid = scheduler_wait_child(current_process, status);
        if (pid != 0) return pid;

        /* Live children but nothing collectable: park until one exits, then
         * rescan. ECHILD (-1) and a collected pid are both returned directly,
         * so only the block case reaches scheduler_block_current(). */
        scheduler_block_current();
    }
}
