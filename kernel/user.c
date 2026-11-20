/*
 * ARC OS - user process loader.
 * Copies the embedded user-shell blob into fresh frames mapped at the
 * user virtual addresses, sets up the user stack and the per-process
 * kernel stack, then registers it with the scheduler.
 *
 * Every frame run is registered on the process as soon as it is allocated,
 * so scheduler_reap() can release the whole process on exit. The error path
 * uses kill+reap instead of manual PMM frees for the same reason.
 */
#include "kernel.h"

int user_process_create_from_blob(void) {
    size_t size = (size_t)_binary_user_shell_bin_size;
    if (size == 0) return -1;

    size_t pages = (size + 4095) / 4096;
    if (pages == 0 || pages > USER_FRAME_PAGES) return -1;

    /*
     * process_create_user() queues the new process immediately. A timer tick
     * during the setup below would switch to it before its code and stack
     * are mapped, producing a user-page fault on the first instruction.
     * Keep the whole window non-preemptible and publish the process only
     * once every mapping is in place.
     */
    preempt_disable();

    int pid = process_create_user(USER_CODE_VA, 0, "shell");
    int ready = 0;

    if (pid >= 0) {
        uint8_t* phys = (uint8_t*)process_alloc_owned_frames(pid, pages);
        if (phys) {
            memset(phys, 0, pages * 4096);
            memcpy(phys, _binary_user_shell_bin_start, size);
            vmm_create_user_mapping(USER_CODE_VA, (uint64_t)phys, pages);

            uint8_t* stk =
                (uint8_t*)process_alloc_owned_frames(pid, USER_STACK_PAGES);
            if (stk) {
                memset(stk, 0, USER_STACK_PAGES * 4096);
                vmm_create_user_mapping(USER_STACK_VA, (uint64_t)stk,
                                        USER_STACK_PAGES);
                ready = 1;
            }
        }

        if (!ready) {
            /* Reap frees the kernel stack and every frame attached above. */
            scheduler_kill(pid);
            scheduler_reap(pid);
        }
    }

    preempt_enable();
    return ready ? pid : -1;
}
