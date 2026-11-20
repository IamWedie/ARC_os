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
    /* Must be first: a timer here could let another syscall overwrite the
     * single global frame pointer before this caller has captured it. */
    preempt_disable();

    int parent_pid = current_process;
    uint64_t frame_rsp = syscall_frame_rsp;

    if (parent_pid <= 0 || parent_pid >= MAX_PROCESSES ||
        !processes[parent_pid].user_mode || frame_rsp == 0) {
        preempt_enable();
        return -1;
    }

    const syscall_frame_t* frame =
        (const syscall_frame_t*)(uintptr_t)frame_rsp;

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
        /* Poll and publish the BLOCKED state as one atomic step: a child exit
         * cannot slip between the scan and the block and lose its wakeup. */
        preempt_disable();
        int pid = scheduler_wait_child(current_process, status);
        if (pid != 0) {
            preempt_enable();
            return pid;
        }

        /* schedule() is a no-op while preemption is disabled, so the block
         * happens under the window but the yield happens after it closes. */
        int blocked = scheduler_block(current_process);
        preempt_enable();
        if (blocked == 0) scheduler_yield();
    }
}
