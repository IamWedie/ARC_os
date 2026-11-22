/*
 * ARC OS - exec image loader and user argument stack.
 *
 * The image and its argument stack are built in fresh frames before the
 * address space is swapped, so every failure path can release only the new
 * allocations and leave the running process untouched.
 */
#include "kernel.h"

/*
 * Write argc/argv at the top of a user stack and return the user VA of the
 * argc word. Strings are copied top-down, the pointer array is 8-byte aligned,
 * and both the returned pointer and the argv entries are user VAs (not the
 * kernel's identity addresses) because _start dereferences them in ring 3.
 * Zero means the arguments could not be placed.
 */
uint64_t exec_build_user_stack(uint8_t* stack_base, size_t stack_bytes,
                               char* const argv[], int argc) {
    if (!stack_base || stack_bytes < 8 || argc < 0 || argc > EXEC_MAX_ARGS)
        return 0;
    if (argc > 0 && !argv) return 0;

    uint8_t* const top = stack_base + stack_bytes;
    uint8_t* sp = top;
    uint64_t arg_vas[EXEC_MAX_ARGS];

    /* Bounded scan: a missing NUL or an overlong argument is rejected. */
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return 0;
        size_t len = 0;
        while (len <= EXEC_MAX_ARG_LEN && argv[i][len] != '\0') len++;
        if (len == 0 || len > EXEC_MAX_ARG_LEN) return 0;

        size_t n = len + 1;
        if (n > (size_t)(sp - stack_base)) return 0;
        sp -= n;
        memcpy(sp, argv[i], n);
        arg_vas[i] = USER_STACK_TOP - (uint64_t)(top - sp);
    }

    /* Align down after the strings; stack_base itself need not be aligned. */
    sp = (uint8_t*)((uintptr_t)sp & ~(uintptr_t)7);
    if (sp < stack_base) return 0;

    /* argc word + argv pointers + trailing NULL. */
    size_t words_bytes = (size_t)(argc + 2) * 8u;
    if (words_bytes > (size_t)(sp - stack_base)) return 0;
    sp -= words_bytes;

    uint64_t* words = (uint64_t*)sp;
    words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) words[1 + (size_t)i] = arg_vas[i];
    words[1 + (size_t)argc] = 0;

    return USER_STACK_TOP - (uint64_t)(top - sp);
}

/*
 * Replace pid's code and stack frames atomically. No owned[] slot is touched
 * until as_replace_user has committed the new root; on failure the saved runs
 * are restored and only the new code/stack frames are released.
 */
int exec_load_image(int pid, const uint8_t* image, size_t size,
                    char* const argv[], int argc, uint64_t* user_rsp_out) {
    if (pid <= 0 || pid >= MAX_PROCESSES) return -1;
    /* Both global caps apply; FS_MAX_FILESIZE is the tighter one today. */
    if (!image || size == 0 || size > (size_t)FS_MAX_FILESIZE ||
        size > (size_t)USER_FRAME_PAGES * 4096u) return -1;
    if (!user_rsp_out) return -1;
    if (argc < 0 || argc > EXEC_MAX_ARGS || (argc > 0 && !argv)) return -1;
    if (!processes[pid].pml4_phys) return -1;

    size_t code_pages = (size + 4095u) / 4096u;
    uint8_t* code = (uint8_t*)pmm_alloc_pages(code_pages);
    if (!code) return -1;

    uint8_t* stack = (uint8_t*)pmm_alloc_pages(USER_STACK_PAGES);
    if (!stack) {
        pmm_free_pages(code, code_pages);
        return -1;
    }

    memset(code, 0, code_pages * 4096u);
    memset(stack, 0, USER_STACK_PAGES * 4096u);
    memcpy(code, image, size);

    uint64_t rsp = exec_build_user_stack(stack, USER_STACK_PAGES * 4096u,
                                         argv, argc);
    if (!rsp) goto fail;

    proc_region_t saved[PROCESS_MAX_OWNED_REGIONS];
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        saved[i] = processes[pid].owned[i];
        processes[pid].owned[i].base = 0;
        processes[pid].owned[i].pages = 0;
    }

    if (as_replace_user(pid, (uint64_t)(uintptr_t)code, code_pages,
                        (uint64_t)(uintptr_t)stack, USER_STACK_PAGES) != 0) {
        for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++)
            processes[pid].owned[i] = saved[i];
        goto fail;
    }

    /* The old roots and tables are already gone; their frames are ours now. */
    for (int i = 0; i < PROCESS_MAX_OWNED_REGIONS; i++) {
        if (saved[i].base)
            pmm_free_pages((void*)(uintptr_t)saved[i].base, (size_t)saved[i].pages);
    }

    processes[pid].owned[0].base = (uint64_t)(uintptr_t)code;
    processes[pid].owned[0].pages = (uint64_t)code_pages;
    processes[pid].owned[1].base = (uint64_t)(uintptr_t)stack;
    processes[pid].owned[1].pages = USER_STACK_PAGES;
    *user_rsp_out = rsp;
    return 0;

fail:
    pmm_free_pages(stack, USER_STACK_PAGES);
    pmm_free_pages(code, code_pages);
    return -1;
}
