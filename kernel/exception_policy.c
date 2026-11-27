/*
 * ARC OS - CPU exception disposition policy.
 *
 * Kept separate from the diagnostic/panic handler so the ring-3 isolation
 * contract can be exercised by host tests against the real scheduler. The
 * origin decision uses the CS selector saved in the interrupt frame, except
 * that #DF is always fatal: a kernel fault that happens while a user process
 * is scheduled still has CS.RPL == 0 and must remain fatal.
 */
#include "kernel.h"

int exception_frame_is_user(const exception_frame_t* frame) {
    return frame != NULL && (frame->cs & 3ULL) == 3ULL;
}

int exception_frame_is_fatal(const exception_frame_t* frame) {
    if (!frame) return 1;
    if ((frame->vec & 0xFFULL) == DOUBLE_FAULT_VECTOR) return 1;
    return !exception_frame_is_user(frame);
}

int exception_exit_code(uint64_t vec) {
    return 128 + (int)(vec & 0xFFULL);
}

uint64_t exception_user_fault_exit(const exception_frame_t* frame,
                                   uint64_t frame_rsp) {
    if (!exception_frame_is_user(frame)) return 0;

    int pid = current_process;
    if (pid <= 0 || pid >= MAX_PROCESSES) return 0;
    if (!processes[pid].user_mode) return 0;

    /* Zombie + wake-parent flow; as_destroy/reap stay the only cleanup path. */
    scheduler_exit_process(pid, exception_exit_code(frame->vec));

    uint64_t next_rsp = scheduler_switch(frame_rsp);
    if (next_rsp == 0 || next_rsp == frame_rsp) return 0;
    return next_rsp;
}
