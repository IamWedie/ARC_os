/*
 * ARC OS - bounded user-memory boundary.
 *
 * Every byte copied across the syscall boundary is first checked against the
 * calling process's live page-table walk. No arbitrary address range is
 * assumed: canonical form and private PML4 slots come from the same paging
 * model used by the VMM. Large pages are rejected because the copy path needs
 * a 4 KiB leaf address.
 */
#include "kernel.h"

#define UA_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define UA_PAGE_MASK (~(uint64_t)4095ULL)

/* Low-half canonical addresses have bits 63:47 clear. */
#define UA_CANONICAL_SHIFT 47

static uint64_t ua_current_root(void) {
    if (current_process < 0 || current_process >= MAX_PROCESSES) return 0;
    return processes[current_process].pml4_phys;
}

/*
 * Bit 7 is PS in PML4E/PDPTE/PDE (where large/huge pages are supported) and
 * PAT in a final 4 KiB PTE. Reject PS above the leaf because this walk needs
 * a 4 KiB address; ignore PAT at the leaf because it only selects caching.
 */
static int ua_entry_ok(uint64_t entry, int write, int is_pte) {
    uint64_t required = PT_PG_PRESENT | PT_PG_USER;
    if ((entry & required) != required) return -1;
    if (!is_pte && (entry & PT_PG_PSE)) return -1;
    if (write && !(entry & PT_PG_RW)) return -1;
    return 0;
}

/*
 * Return the 4 KiB leaf entry for `page_va`, or NULL if any level is absent,
 * kernel-only, or (for writes) read-only. Physical table addresses are
 * identity-mapped in the kernel's slot 0, as in the rest of the paging core.
 */
static uint64_t* ua_walk(uint64_t root_phys, uint64_t page_va, int write) {
    if ((page_va >> 39) < PT_USER_SLOT_FIRST) return NULL;

    uint64_t* table = (uint64_t*)(uintptr_t)root_phys;
    uint64_t entry = table[(page_va >> 39) & 0x1FF];
    if (ua_entry_ok(entry, write, 0) != 0) return NULL;

    table = (uint64_t*)(uintptr_t)(entry & UA_ADDR_MASK);
    entry = table[(page_va >> 30) & 0x1FF];
    if (ua_entry_ok(entry, write, 0) != 0) return NULL;

    table = (uint64_t*)(uintptr_t)(entry & UA_ADDR_MASK);
    entry = table[(page_va >> 21) & 0x1FF];
    if (ua_entry_ok(entry, write, 0) != 0) return NULL;

    table = (uint64_t*)(uintptr_t)(entry & UA_ADDR_MASK);
    entry = table[(page_va >> 12) & 0x1FF];
    if (ua_entry_ok(entry, write, 1) != 0) return NULL;

    return &table[(page_va >> 12) & 0x1FF];
}

/* Validate every page in [va, va+len) before any byte is touched. */
static int ua_validate(uint64_t root_phys, uint64_t va, uint64_t len,
                       int write) {
    if (len == 0) return 0;
    if (len > UINT64_MAX - va) return -1;

    uint64_t last = va + len - 1;
    if ((va >> UA_CANONICAL_SHIFT) || (last >> UA_CANONICAL_SHIFT)) return -1;

    uint64_t page = va & UA_PAGE_MASK;
    uint64_t last_page = last & UA_PAGE_MASK;
    for (;;) {
        if ((page >> 39) < PT_USER_SLOT_FIRST) return -1;
        if (ua_walk(root_phys, page, write) == NULL) return -1;
        if (page == last_page) break;
        page += 4096ULL;
    }
    return 0;
}

int user_range_readable(const void* user_src, size_t len) {
    if (len == 0) return 0;
    if (!user_src) return -1;

    uint64_t root_phys = ua_current_root();
    if (!root_phys) return -1;
    return ua_validate(root_phys, (uint64_t)(uintptr_t)user_src,
                       (uint64_t)len, 0);
}

int user_range_writable(void* user_dst, size_t len) {
    if (len == 0) return 0;
    if (!user_dst) return -1;

    uint64_t root_phys = ua_current_root();
    if (!root_phys) return -1;
    return ua_validate(root_phys, (uint64_t)(uintptr_t)user_dst,
                       (uint64_t)len, 1);
}

int copy_from_user(void* dst, const void* user_src, size_t len) {
    if (len == 0) return 0;
    if (!dst || !user_src) return -1;

    uint64_t root_phys = ua_current_root();
    if (!root_phys) return -1;

    uint64_t va = (uint64_t)(uintptr_t)user_src;
    uint64_t left = (uint64_t)len;
    if (ua_validate(root_phys, va, left, 0) != 0) return -1;

    uint8_t* out = (uint8_t*)dst;
    while (left) {
        uint64_t* leaf = ua_walk(root_phys, va & UA_PAGE_MASK, 0);
        if (!leaf) return -1;

        uint64_t offset = va & 4095ULL;
        uint64_t chunk = 4096ULL - offset;
        if (chunk > left) chunk = left;

        uint64_t pa = (*leaf & UA_ADDR_MASK) + offset;
        memcpy(out, (const void*)(uintptr_t)pa, (size_t)chunk);

        out += chunk;
        va += chunk;
        left -= chunk;
    }
    return 0;
}

int copy_to_user(void* user_dst, const void* src, size_t len) {
    if (len == 0) return 0;
    if (!user_dst || !src) return -1;

    uint64_t root_phys = ua_current_root();
    if (!root_phys) return -1;

    uint64_t va = (uint64_t)(uintptr_t)user_dst;
    uint64_t left = (uint64_t)len;
    if (ua_validate(root_phys, va, left, 1) != 0) return -1;

    const uint8_t* in = (const uint8_t*)src;
    while (left) {
        uint64_t* leaf = ua_walk(root_phys, va & UA_PAGE_MASK, 1);
        if (!leaf) return -1;

        uint64_t offset = va & 4095ULL;
        uint64_t chunk = 4096ULL - offset;
        if (chunk > left) chunk = left;

        uint64_t pa = (*leaf & UA_ADDR_MASK) + offset;
        memcpy((void*)(uintptr_t)pa, in, (size_t)chunk);

        in += chunk;
        va += chunk;
        left -= chunk;
    }
    return 0;
}

/* Find the NUL without writing dst, then copy only the validated prefix. */
int copy_user_string(const void* user_src, char* dst, size_t cap) {
    if (cap == 0 || cap > (size_t)0x7FFFFFFFu || !user_src || !dst) return -1;

    uint64_t va = (uint64_t)(uintptr_t)user_src;
    for (size_t i = 0; i < cap; i++) {
        if ((uint64_t)i > UINT64_MAX - va) return -1;
        char c;
        if (copy_from_user(&c, (const void*)(uintptr_t)(va + (uint64_t)i), 1)
            != 0) return -1;
        if (c == '\0') {
            if (copy_from_user(dst, user_src, i + 1) != 0) return -1;
            return (int)i;
        }
    }
    return -1;
}
