/*
 * ARC OS - user process loader.
 * Copies the embedded user-shell blob into fresh frames mapped at the
 * user virtual addresses, sets up the user stack and the per-process
 * kernel stack, then registers it with the scheduler.
 */
#include "kernel.h"

int user_process_create_from_blob(void) {
    size_t size = (size_t)_binary_user_shell_bin_size;
    if (size == 0) return -1;

    size_t pages = (size + 4095) / 4096;
    if (pages == 0 || pages > USER_FRAME_PAGES) return -1;

    uint8_t* phys = (uint8_t*)pmm_alloc_pages(pages);
    if (!phys) return -1;
    memset(phys, 0, pages * 4096);
    memcpy(phys, _binary_user_shell_bin_start, size);
    vmm_create_user_mapping(USER_CODE_VA, (uint64_t)phys, pages);

    uint8_t* stk = (uint8_t*)pmm_alloc_pages(USER_STACK_PAGES);
    if (!stk) return -1;
    memset(stk, 0, USER_STACK_PAGES * 4096);
    vmm_create_user_mapping(USER_STACK_VA, (uint64_t)stk, USER_STACK_PAGES);

    void* kstack = pmm_alloc_pages(4);
    if (!kstack) return -1;
    uint64_t kstack_top = (uint64_t)kstack + 4 * 4096;

    return process_create_user(USER_CODE_VA, kstack_top, "shell");
}