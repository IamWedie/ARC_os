/*
 * ARC OS - Page-table walk core.
 *
 * PML4 -> PDPT -> PD -> PT manipulation over plain, identity-accessible
 * memory. There are no privileged instructions and no implicit PMM use:
 * every table comes from the caller's pt_ops_t. That keeps the walk host-
 * testable and lets the kernel VMM and per-process address spaces share a
 * single implementation.
 */
#include "kernel.h"

#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint64_t pt_flags(uint64_t flags) {
    return PT_PG_PRESENT | (flags ? flags : PT_PG_RW);
}

static uint64_t pt_index(uint64_t vaddr, int shift) {
    return (vaddr >> shift) & 0x1FF;
}

static uint64_t* pt_table(uint64_t entry) {
    return (uint64_t*)(uintptr_t)(entry & PT_ADDR_MASK);
}

/*
 * Return the table one level below `table`, allocating and linking a zeroed
 * page when the entry is absent. NULL means a large page is already mapped
 * over the range or the allocator failed; existing levels are left intact so
 * the root stays usable.
 */
static uint64_t* pt_descend(uint64_t* table, uint64_t vaddr, int shift,
                            uint64_t flags, const pt_ops_t* ops) {
    uint64_t* entry = &table[pt_index(vaddr, shift)];
    if (*entry & PT_PG_PSE) return NULL;
    if (!(*entry & PT_PG_PRESENT)) {
        uint64_t* page = ops->alloc();
        if (!page) return NULL;
        *entry = ((uint64_t)(uintptr_t)page & PT_ADDR_MASK) | flags;
    }
    return pt_table(*entry);
}

/* Non-allocating hop used by lookup: NULL when the level is absent or large. */
static uint64_t* pt_follow(const uint64_t* table, uint64_t vaddr, int shift) {
    uint64_t entry = table[pt_index(vaddr, shift)];
    if (!(entry & PT_PG_PRESENT) || (entry & PT_PG_PSE)) return NULL;
    return (uint64_t*)(uintptr_t)(entry & PT_ADDR_MASK);
}

int pt_map(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags,
           const pt_ops_t* ops) {
    if (!root || !ops || !ops->alloc) return -1;

    uint64_t fl = pt_flags(flags);
    uint64_t* pdpt = pt_descend(root, vaddr, 39, fl, ops);
    if (!pdpt) return -1;
    uint64_t* pd = pt_descend(pdpt, vaddr, 30, fl, ops);
    if (!pd) return -1;
    uint64_t* pt = pt_descend(pd, vaddr, 21, fl, ops);
    if (!pt) return -1;

    pt[pt_index(vaddr, 12)] = (paddr & PT_ADDR_MASK) | fl;
    return 0;
}

uint64_t* pt_lookup(uint64_t* root, uint64_t vaddr) {
    if (!root) return NULL;
    uint64_t* pdpt = pt_follow(root, vaddr, 39);
    if (!pdpt) return NULL;
    uint64_t* pd = pt_follow(pdpt, vaddr, 30);
    if (!pd) return NULL;
    uint64_t* pt = pt_follow(pd, vaddr, 21);
    if (!pt) return NULL;

    uint64_t* leaf = &pt[pt_index(vaddr, 12)];
    return (*leaf & PT_PG_PRESENT) ? leaf : NULL;
}

int pt_unmap(uint64_t* root, uint64_t vaddr) {
    uint64_t* leaf = pt_lookup(root, vaddr);
    if (!leaf) return -1;
    *leaf = 0;
    return 0;
}

/*
 * Free every table reachable from `table[first..511]`, then unlink each entry.
 * level 0 = PML4, 1 = PDPT, 2 = PD: a PD's children are page tables whose
 * entries are mapped pages, so recursion stops before those leaves. Large-page
 * entries are mappings, not tables, and are left alone.
 */
static void pt_free_children(uint64_t* table, int first, int level,
                             const pt_ops_t* ops) {
    for (int i = first; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PT_PG_PRESENT) || (entry & PT_PG_PSE)) continue;
        uint64_t* child = pt_table(entry);
        if (!child) continue;

        if (level < 2) pt_free_children(child, 0, level + 1, ops);

        table[i] = 0; /* unlink before free: a second teardown sees nothing */
        ops->free(child);
    }
}

void pt_clone_shared(uint64_t* src, uint64_t* dst) {
    if (!src || !dst) return;

    dst[0] = src[0];
    for (int i = PT_USER_SLOT_FIRST; i < 512; i++) dst[i] = 0;
}

/*
 * Reclaim the private tables of a process root without touching the root or
 * the shared kernel slot 0. Zeroing on the way down also makes repeated calls
 * harmless even if the allocator does not clear freed pages.
 */
void pt_free_user_tables(uint64_t* root, const pt_ops_t* ops) {
    if (!root || !ops || !ops->free) return;
    pt_free_children(root, PT_USER_SLOT_FIRST, 0, ops);
}

/*
 * Count the 4 KiB leaves mapped in the private (user) half of `root`. PML4
 * slot 0 is the shared kernel identity map and is never counted; large pages
 * are mappings of their own, not leaves, so they are not counted either.
 */
int pt_count_user_pages(const uint64_t* root) {
    if (!root) return 0;

    int pages = 0;
    for (int i = PT_USER_SLOT_FIRST; i < 512; i++) {
        uint64_t pml4e = root[i];
        if (!(pml4e & PT_PG_PRESENT) || (pml4e & PT_PG_PSE)) continue;
        const uint64_t* pdpt =
            (const uint64_t*)(uintptr_t)(pml4e & PT_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & PT_PG_PRESENT) || (pdpte & PT_PG_PSE)) continue;
            const uint64_t* pd =
                (const uint64_t*)(uintptr_t)(pdpte & PT_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = pd[k];
                if (!(pde & PT_PG_PRESENT) || (pde & PT_PG_PSE)) continue;
                const uint64_t* pt =
                    (const uint64_t*)(uintptr_t)(pde & PT_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PT_PG_PRESENT) pages++;
                }
            }
        }
    }
    return pages;
}

/*
 * Walk the same leaves as pt_count_user_pages() and report each one to cb.
 * Large pages are mappings rather than 4 KiB leaves, so they are skipped;
 * only the user-settable bits are passed through to the caller.
 */
void pt_foreach_user_leaf(const uint64_t* root,
                          void (*cb)(uint64_t va, uint64_t pa, uint64_t flags,
                                     void* ctx),
                          void* ctx) {
    if (!root || !cb) return;

    for (int i = PT_USER_SLOT_FIRST; i < 512; i++) {
        uint64_t pml4e = root[i];
        if (!(pml4e & PT_PG_PRESENT) || (pml4e & PT_PG_PSE)) continue;
        const uint64_t* pdpt =
            (const uint64_t*)(uintptr_t)(pml4e & PT_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & PT_PG_PRESENT) || (pdpte & PT_PG_PSE)) continue;
            const uint64_t* pd =
                (const uint64_t*)(uintptr_t)(pdpte & PT_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = pd[k];
                if (!(pde & PT_PG_PRESENT) || (pde & PT_PG_PSE)) continue;
                const uint64_t* pt =
                    (const uint64_t*)(uintptr_t)(pde & PT_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    uint64_t entry = pt[l];
                    if (!(entry & PT_PG_PRESENT) || (entry & PT_PG_PSE))
                        continue;
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    cb(va, entry & PT_ADDR_MASK,
                       entry & (PT_PG_RW | PT_PG_USER), ctx);
                }
            }
        }
    }
}
