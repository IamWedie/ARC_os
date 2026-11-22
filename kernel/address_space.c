/*
 * ARC OS - Per-process address spaces.
 *
 * Turns PMM pages into private PML4 roots whose slot 0 is copied from the
 * master kernel map and whose slots 1..511 stay private to the process. All
 * table walking is delegated to kernel/pagetable.c; this module only owns the
 * process table entry, the PMM lifecycle, and the invalidate hook.
 */
#include "kernel.h"

/*
 * The PMM does not clear recycled frames, and the page-table core requires a
 * zeroed page from ops->alloc. Use the same wrapper for the root and for every
 * intermediate PDPT/PD/PT page so a reused frame cannot smuggle stale entries
 * into a new address space.
 */
static void* as_alloc_table(void) {
    void* page = pmm_alloc_pages(1);
    if (page) {
        uint64_t* words = (uint64_t*)page;
        for (size_t i = 0; i < 4096 / sizeof(*words); i++) words[i] = 0;
    }
    return page;
}

static void as_free_table(void* page) {
    if (page) pmm_free_pages(page, 1);
}

static const pt_ops_t as_ops = {
    .alloc = as_alloc_table,
    .free  = as_free_table,
};

static int as_valid_pid(int pid) {
    return pid > 0 && pid < MAX_PROCESSES;
}

int as_create(int pid) {
    if (!as_valid_pid(pid) || processes[pid].pml4_phys) return -1;

    uint64_t master = vmm_master_root();
    if (!master) return -1;

    void* root = as_alloc_table();
    if (!root) return -1;

    pt_clone_shared((uint64_t*)(uintptr_t)master, (uint64_t*)root);
    processes[pid].pml4_phys = (uint64_t)(uintptr_t)root;
    return 0;
}

uint64_t as_root(int pid) {
    if (!as_valid_pid(pid)) return 0;    /* idle and kernel threads use master */
    return processes[pid].pml4_phys;
}

int as_map_user(int pid, uint64_t vaddr, uint64_t paddr,
                size_t pages, uint8_t flags) {
    if (!as_valid_pid(pid)) return -1;

    uint64_t root_phys = processes[pid].pml4_phys;
    if (!root_phys) return -1;
    if ((vaddr >> 39) < PT_USER_SLOT_FIRST) return -1; /* slot 0 is kernel */

    uint64_t* root = (uint64_t*)(uintptr_t)root_phys;
    uint64_t user_flags = (uint64_t)flags | PT_PG_USER;

    for (size_t i = 0; i < pages; i++) {
        uint64_t va = vaddr + (uint64_t)i * 4096ULL;
        if (pt_map(root, va, paddr + (uint64_t)i * 4096ULL,
                   user_flags, &as_ops) != 0)
            return -1;
        vmm_invalidate_page(va);
    }
    return 0;
}

/*
 * Copy one source leaf into the next page of the child's contiguous run.
 * `failed` is sticky: the walk continues but performs no further work once a
 * copy or map fails. The child root and the registered run are deliberately
 * left in place so scheduler_reap() can release them on the caller's -1 path.
 */
typedef struct {
    uint64_t* root;
    uint8_t* run;
    size_t size;
    size_t used;
    int failed;
} as_clone_ctx_t;

static void as_clone_copy_leaf(uint64_t va, uint64_t pa, uint64_t flags,
                               void* ctx) {
    as_clone_ctx_t* c = (as_clone_ctx_t*)ctx;
    if (c->failed || c->used + 4096 > c->size) {
        c->failed = 1;
        return;
    }

    uint8_t* dst = c->run + c->used;
    memcpy(dst, (const void*)(uintptr_t)pa, 4096);
    if (pt_map(c->root, va, (uint64_t)(uintptr_t)dst,
               flags | PT_PG_USER, &as_ops) != 0) {
        c->failed = 1;
        return;
    }
    c->used += 4096;
}

int as_clone(int src_pid, int dst_pid) {
    if (!as_valid_pid(src_pid) || !as_valid_pid(dst_pid)) return -1;
    if (src_pid == dst_pid) return -1;

    uint64_t src_root_phys = processes[src_pid].pml4_phys;
    if (!src_root_phys || processes[dst_pid].pml4_phys) return -1;
    if (processes[dst_pid].owned[0].base) return -1;

    if (as_create(dst_pid) != 0) return -1;
    uint64_t dst_root_phys = processes[dst_pid].pml4_phys;

    size_t pages =
        (size_t)pt_count_user_pages((const uint64_t*)(uintptr_t)src_root_phys);
    if (pages == 0) return 0; /* empty user half: no frames to copy */

    void* run = pmm_alloc_pages(pages);
    if (!run) return -1; /* child root stays valid for the caller to reap */

    processes[dst_pid].owned[0].base = (uint64_t)(uintptr_t)run;
    processes[dst_pid].owned[0].pages = (uint64_t)pages;

    as_clone_ctx_t ctx = {
        .root = (uint64_t*)(uintptr_t)dst_root_phys,
        .run  = (uint8_t*)run,
        .size = pages * 4096u,
        .used = 0,
        .failed = 0,
    };
    pt_foreach_user_leaf((const uint64_t*)(uintptr_t)src_root_phys,
                         as_clone_copy_leaf, &ctx);
    return ctx.failed ? -1 : 0;
}

/*
 * Build the replacement user half in a detached root and only publish it once
 * every mapping succeeded. Zero is not a real frame: rejecting it here keeps a
 * failed replacement from leaving a bogus present leaf behind. Nothing in this
 * function touches the process's owned[] frame runs.
 */
int as_replace_user(int pid, uint64_t code_pa, size_t code_pages,
                    uint64_t stack_pa, size_t stack_pages) {
    if (!as_valid_pid(pid) || !code_pa || !stack_pa) return -1;

    uint64_t old_root_phys = processes[pid].pml4_phys;
    uint64_t master = vmm_master_root();
    if (!old_root_phys || !master) return -1;

    void* root = as_alloc_table();
    if (!root) return -1;

    uint64_t* new_root = (uint64_t*)root;
    pt_clone_shared((uint64_t*)(uintptr_t)master, new_root);

    for (size_t i = 0; i < code_pages; i++) {
        if (pt_map(new_root, USER_CODE_VA + (uint64_t)i * 4096ULL,
                   code_pa + (uint64_t)i * 4096ULL,
                   PT_PG_RW | PT_PG_USER, &as_ops) != 0)
            goto fail;
    }
    for (size_t i = 0; i < stack_pages; i++) {
        if (pt_map(new_root, USER_STACK_VA + (uint64_t)i * 4096ULL,
                   stack_pa + (uint64_t)i * 4096ULL,
                   PT_PG_RW | PT_PG_USER, &as_ops) != 0)
            goto fail;
    }

    processes[pid].pml4_phys = (uint64_t)(uintptr_t)root;
    vmm_switch_address_space(pid); /* new CR3 before old tables are freed */

    uint64_t* old_root = (uint64_t*)(uintptr_t)old_root_phys;
    pt_free_user_tables(old_root, &as_ops);
    as_free_table(old_root);
    return 0;

fail:
    pt_free_user_tables(new_root, &as_ops); /* only the detached root's tables */
    as_free_table(root);
    return -1;
}

void as_destroy(int pid) {
    if (!as_valid_pid(pid)) return;

    uint64_t root_phys = processes[pid].pml4_phys;
    if (!root_phys) return;
    processes[pid].pml4_phys = 0; /* first: a second destroy is a no-op */

    uint64_t* root = (uint64_t*)(uintptr_t)root_phys;
    pt_free_user_tables(root, &as_ops); /* never touches shared slot 0 */
    as_free_table(root);
}
