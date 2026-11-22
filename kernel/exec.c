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

/* ------------------------------------------------------------------ */
/* SYS_EXEC: checked user arguments + live trap frame rewrite          */
/* ------------------------------------------------------------------ */

#define EXEC_PT_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define EXEC_PAGE_MASK    (~(uint64_t)4095ULL)

/*
 * Copy `len` bytes from a user VA whose every 4 KiB page must have a present,
 * user-accessible leaf in the caller's root. There is no page-fault handler,
 * so an unchecked dereference here would fault in kernel mode. The physical
 * leaf address is identity-mapped in the kernel's slot 0.
 */
static int exec_read_user(uint64_t root_phys, uint64_t va, void* dst,
                          size_t len) {
    uint8_t* out = (uint8_t*)dst;
    if (!out && len) return -1;
    if (len > (size_t)(UINT64_MAX - va)) return -1;

    while (len) {
        uint64_t page_va = va & EXEC_PAGE_MASK;
        uint64_t* leaf = pt_lookup((uint64_t*)(uintptr_t)root_phys, page_va);
        if (!leaf || !(*leaf & PT_PG_PRESENT) || !(*leaf & PT_PG_USER) ||
            (*leaf & PT_PG_PSE)) return -1;

        uint64_t offset = va & 4095ULL;
        uint64_t pa = (*leaf & EXEC_PT_ADDR_MASK) + offset;
        size_t n = (size_t)(4096ULL - offset);
        if (n > len) n = len;
        memcpy(out, (const void*)(uintptr_t)pa, n);

        out += n;
        va += n;
        len -= n;
    }
    return 0;
}

/* Copy at most cap bytes (including the NUL) from a user C string. */
static int exec_copy_user_cstr(uint64_t root_phys, uint64_t va, char* buf,
                               size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        char c;
        if (exec_read_user(root_phys, va + i, &c, 1) != 0) return -1;
        if (c == '\0') {
            buf[i] = '\0';
            return (int)i;
        }
        buf[i] = c;
    }
    return -1;
}

/*
 * Copy at most EXEC_MAX_ARGS user pointers and their strings into kernel stack
 * buffers, then read one more slot for the required NULL terminator. That extra
 * read is what lets a full 8-argument argv succeed. A missing terminator, an
 * overlong/empty string or a bad pointer is rejected before any process state
 * is touched.
 */
static int exec_copy_user_argv(uint64_t root_phys, uint64_t argv_va,
                               char* argbuf[EXEC_MAX_ARGS],
                               char storage[EXEC_MAX_ARGS][EXEC_MAX_ARG_LEN + 1],
                               int* argc_out) {
    for (int i = 0; i <= EXEC_MAX_ARGS; i++) {
        uint64_t ptr = 0;
        if (exec_read_user(root_phys,
                           argv_va + (uint64_t)(size_t)i * 8u,
                           &ptr, 8) != 0) return -1;
        if (ptr == 0) {
            *argc_out = i;
            return 0;
        }
        if (i == EXEC_MAX_ARGS) return -1; /* more than EXEC_MAX_ARGS args */
        int len = exec_copy_user_cstr(root_phys, ptr, storage[i],
                                      EXEC_MAX_ARG_LEN + 1u);
        if (len <= 0 || len > EXEC_MAX_ARG_LEN) return -1;
        argbuf[i] = storage[i];
    }
    return -1; /* unreachable: the loop reads the terminator at slot argc */
}

long sys_exec(const char* path, char* const argv[]) {
    /* Must be first: a timer here could let another syscall overwrite the
     * single global frame pointer before this caller has captured it. */
    preempt_disable();

    int pid = current_process;
    uint64_t frame_rsp = syscall_frame_rsp;
    if (pid <= 0 || pid >= MAX_PROCESSES || !processes[pid].user_mode ||
        frame_rsp == 0 || !path || !argv) goto fail;

    uint64_t root_phys = processes[pid].pml4_phys;
    if (!root_phys) goto fail;

    char path_buf[FS_MAX_FILENAME];
    char arg_storage[EXEC_MAX_ARGS][EXEC_MAX_ARG_LEN + 1];
    char* argbuf[EXEC_MAX_ARGS];
    int argc = 0;
    if (exec_copy_user_cstr(root_phys, (uint64_t)(uintptr_t)path, path_buf,
                            sizeof(path_buf)) <= 0 ||
        exec_copy_user_argv(root_phys, (uint64_t)(uintptr_t)argv, argbuf,
                            arg_storage, &argc) != 0) goto fail;

    int slot = fs_find_slot(path_buf);
    if (slot < 0) goto fail;
    int file_size = fs_slot_size(slot);
    if (file_size <= 0 || (size_t)file_size > (size_t)FS_MAX_FILESIZE ||
        (size_t)file_size > (size_t)USER_FRAME_PAGES * 4096u) goto fail;

    /* Stage the whole file in fresh frames; exec_load_image copies it into
     * its own code run and these frames are released on every path. */
    size_t size = (size_t)file_size;
    size_t pages = (size + 4095u) / 4096u;
    uint8_t* image = (uint8_t*)pmm_alloc_pages(pages);
    if (!image) goto fail;

    size_t off = 0;
    while (off < size) {
        int n = fs_slot_read(slot, (uint32_t)off, image + off, size - off);
        if (n <= 0 || (size_t)n > size - off) goto fail_stage;
        off += (size_t)n;
    }

    uint64_t user_rsp = 0;
    if (exec_load_image(pid, image, size, argbuf, argc, &user_rsp) != 0)
        goto fail_stage;
    pmm_free_pages(image, pages);

    /* Commit is complete: close-on-exec cannot fail the exec now. */
    for (int fd = FD_STDERR + 1; fd < FD_MAX; fd++) {
        if (processes[pid].fds[fd].kind != FD_ENTRY_FREE) fd_close(pid, fd);
    }

    syscall_frame_t* frame = (syscall_frame_t*)(uintptr_t)frame_rsp;
    frame->rip = USER_CODE_VA;
    frame->user_rsp = user_rsp;
    frame->rax = 0;
    frame->ret_slot = 0;

    preempt_enable();
    return 0;

fail_stage:
    pmm_free_pages(image, pages);
fail:
    preempt_enable();
    return -1;
}

/* ------------------------------------------------------------------ */
/* Boot-time program registry                                          */
/* ------------------------------------------------------------------ */

/*
 * Publish the second embedded flat binary in the RAM FS before the shell
 * starts. The OFD-style ref is acquired and released exactly as in fs_init's
 * welcome.txt seeding, so a boot-time write leaves no slot references behind
 * and does not disturb the one-ref-per-OFD contract.
 */
void exec_install_programs(void) {
    size_t size = (size_t)_binary_user_hello_bin_size;
    if (size == 0 || size > (size_t)FS_MAX_FILESIZE) return;

    int slot = fs_open_slot("hello.bin", 1);
    if (slot < 0) return;
    if (fs_slot_add_ref(slot) != 0) return;

    /* A persisted slot from an older, larger image must not keep its size or
     * trailing bytes: fs_slot_write only ever grows a file. Truncate does not
     * touch open_refs, so the one add-ref / one release pattern still holds. */
    fs_slot_truncate(slot);
    fs_slot_write(slot, 0, _binary_user_hello_bin_start, size);
    fs_slot_release(slot);
}
