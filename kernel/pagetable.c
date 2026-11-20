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
