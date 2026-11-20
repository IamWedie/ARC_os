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

void as_destroy(int pid) {
    if (!as_valid_pid(pid)) return;

    uint64_t root_phys = processes[pid].pml4_phys;
    if (!root_phys) return;
    processes[pid].pml4_phys = 0; /* first: a second destroy is a no-op */

    uint64_t* root = (uint64_t*)(uintptr_t)root_phys;
    pt_free_user_tables(root, &as_ops); /* never touches shared slot 0 */
    as_free_table(root);
}
