/*
 * Host-side page source for the page-table core tests.
 *
 * kernel/pagetable.c never touches hardware: it is handed a root table and a
 * pair of allocation hooks. This stub supplies both from a 4 MiB arena of
 * zeroed 4 KiB pages, so the core can be exercised without a PMM or CR3.
 *
 * alloc hands out the next untouched arena page and zeroes it (the contract
 * the real vmm_ops will honour). free only counts: the core's user-page
 * teardown is verified by page count, not by reuse.
 */
#include <stddef.h>
#include <stdint.h>

#include "kernel.h"

#define PT_STUB_PAGES 1024u /* 4 MiB */

static uint8_t arena[PT_STUB_PAGES * 4096u] __attribute__((aligned(4096)));
static uint64_t root_page[512] __attribute__((aligned(4096)));

static uint32_t next_page;
static int allocs;
static int frees;
static int fail_after = -1; /* -1 disables; otherwise fail from this alloc on */

static void zero_page(uint64_t* page) {
    for (int i = 0; i < 512; i++) page[i] = 0;
}

static void* stub_alloc(void) {
    if (fail_after >= 0 && allocs >= fail_after) return NULL;
    if (next_page >= PT_STUB_PAGES) return NULL;

    uint64_t* page = (uint64_t*)(arena + (size_t)next_page * 4096u);
    next_page++;
    allocs++;
    zero_page(page);
    return page;
}

static void stub_free(void* page) {
    (void)page;
    frees++;
}

pt_ops_t pt_stub_ops = { stub_alloc, stub_free };

uint64_t* pt_stub_root(void) {
    return root_page;
}

void pt_stub_fail_after(int n) {
    fail_after = n;
}

int pt_stub_alloc_count(void) {
    return allocs;
}

int pt_stub_freed_count(void) {
    return frees;
}

void pt_stub_reset(void) {
    uint64_t* words = (uint64_t*)arena;
    for (size_t i = 0; i < sizeof(arena) / sizeof(words[0]); i++) words[i] = 0;
    zero_page(root_page);
    next_page = 0;
    allocs = 0;
    frees = 0;
    fail_after = -1;
}
