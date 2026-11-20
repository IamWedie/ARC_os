/*
 * Host-side PMM stub for slab allocator tests.
 *
 * Provides a 64 MiB page-aligned arena and the same alloc/free contract
 * as kernel/pmm.c (identity-mapped, 4 KiB frames, first fit).
 */
#include <stddef.h>
#include <stdint.h>

#define STUB_PAGES 16384u /* 64 MiB */

static uint8_t  arena[STUB_PAGES * 4096u] __attribute__((aligned(4096)));
static uint8_t  used[STUB_PAGES];
static uint64_t used_pages;

void* pmm_alloc_pages(size_t count) {
    if (count == 0) count = 1;
    size_t run = 0;

    for (size_t i = 0; i < STUB_PAGES; i++) {
        if (!used[i]) {
            run++;
            if (run == count) {
                size_t start = i + 1 - run;
                for (size_t j = start; j < start + count; j++) used[j] = 1;
                used_pages += count;
                return (void*)(arena + start * 4096u);
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

void pmm_free_pages(void* addr, size_t count) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)arena;
    if (a < base || a >= base + sizeof(arena)) return;
    if ((a - base) % 4096u) return;

    size_t start = (size_t)((a - base) / 4096u);
    for (size_t i = 0; i < count && start + i < STUB_PAGES; i++) {
        if (used[start + i]) {
            used[start + i] = 0;
            used_pages--;
        }
    }
}

uint64_t pmm_total_mem(void) { return (uint64_t)STUB_PAGES * 4096u; }
uint64_t pmm_free_mem(void)  { return (uint64_t)(STUB_PAGES - used_pages) * 4096u; }

/* Test-only introspection. */
uint64_t pmm_stub_used_pages(void) { return used_pages; }
void pmm_stub_reset(void) {
    for (size_t i = 0; i < STUB_PAGES; i++) used[i] = 0;
    used_pages = 0;
}
