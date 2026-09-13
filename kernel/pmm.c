/*
 * ARC OS - Physical memory manager.
 *
 * Bitmap over a fixed 16 GiB window of physical memory. The multiboot
 * memory map (or the mem_upper fallback) drives which frames are usable;
 * the low 1 MiB and the resident kernel image are always reserved.
 */
#include "kernel.h"
#include "mboot.h"

#define PMM_PAGE_SIZE      4096ULL
#define PMM_MAX_PHYS       (16ULL << 30)          /* 16 GiB */
#define PMM_MAX_PAGES      (PMM_MAX_PHYS / PMM_PAGE_SIZE)
#define PMM_BITMAP_WORDS   ((PMM_MAX_PAGES + 63) / 64)

#define PMM_MAX_REGIONS    64

/* bit = 1 -> reserved, bit = 0 -> free */
static uint64_t pmm_bitmap[PMM_BITMAP_WORDS];

static uint64_t pmm_total_frames;
static uint64_t pmm_free_frames;
static uint32_t pmm_region_count;

static struct pmm_region pmm_regions[PMM_MAX_REGIONS];

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];

static inline uint64_t pmm_frame_hi(uint64_t frame) {
    return frame >= PMM_MAX_PAGES ? PMM_MAX_PAGES - 1 : frame;
}

static inline void pmm_set(uint64_t frame) {
    pmm_bitmap[frame / 64] |= (1ULL << (frame % 64));
}

static inline void pmm_clear(uint64_t frame) {
    pmm_bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}

static inline int pmm_is_free(uint64_t frame) {
    return (pmm_bitmap[frame / 64] >> (frame % 64)) & 1ULL ? 0 : 1;
}

/* Clamp [base, base+len) to the managed window and mark free (clear). */
static void pmm_release(uint64_t base, uint64_t len) {
    if (base >= PMM_MAX_PHYS) return;
    uint64_t end = base + len;
    if (end > PMM_MAX_PHYS) end = PMM_MAX_PHYS;
    uint64_t f0 = (base + PMM_PAGE_SIZE - 1) >> 12;
    uint64_t f1 = (end >> 12) - 1;
    if (f1 < f0) return;
    for (uint64_t f = pmm_frame_hi(f0); f <= pmm_frame_hi(f1); f++) {
        if (!pmm_is_free(f)) {
            pmm_clear(f);
            pmm_free_frames++;
        }
    }
}

static void pmm_reserve(uint64_t base, uint64_t len) {
    if (base >= PMM_MAX_PHYS) return;
    uint64_t end = base + len;
    if (end > PMM_MAX_PHYS) end = PMM_MAX_PHYS;
    uint64_t f0 = (base + PMM_PAGE_SIZE - 1) >> 12;
    uint64_t f1 = (end >> 12) - 1;
    if (f1 < f0) return;
    for (uint64_t f = pmm_frame_hi(f0); f <= pmm_frame_hi(f1); f++) {
        if (pmm_is_free(f)) {
            pmm_set(f);
            pmm_free_frames--;
        }
    }
}

void pmm_init(uint64_t multiboot_addr) {
    struct multiboot_info* mi = (struct multiboot_info*)multiboot_addr;
    int have_mmap = 0;

    for (size_t i = 0; i < PMM_BITMAP_WORDS; i++)
        pmm_bitmap[i] = ~0ULL;

    if (mi && (mi->flags & MBOOT_FLAG_MMAP) && mi->mmap_length) {
        uintptr_t p = mi->mmap_addr;
        uint32_t left = mi->mmap_length;
        while (left >= sizeof(struct multiboot_mmap_entry) &&
               pmm_region_count < PMM_MAX_REGIONS) {
            struct multiboot_mmap_entry* e =
                (struct multiboot_mmap_entry*)p;
            if (e->type == MBOOT_MMAP_AVAILABLE) {
                pmm_regions[pmm_region_count].base = e->addr;
                pmm_regions[pmm_region_count].len = e->len;
                pmm_regions[pmm_region_count].type = e->type;
                pmm_region_count++;
                pmm_total_frames += e->len >> 12;
                pmm_release(e->addr, e->len);
            }
            uint32_t step = e->size + 4;
            if (step < 20) break;
            p += step;
            left -= (left > step) ? step : left;
            have_mmap = 1;
        }
    }

    if (!have_mmap && mi && (mi->flags & MBOOT_FLAG_MEM)) {
        uint64_t upper = (uint64_t)mi->mem_upper * 1024ULL;
        pmm_regions[pmm_region_count].base = 0x100000;
        pmm_regions[pmm_region_count].len = upper;
        pmm_regions[pmm_region_count].type = MBOOT_MMAP_AVAILABLE;
        pmm_region_count++;
        pmm_total_frames += upper >> 12;
        pmm_release(0x100000, upper);
    }

    /* Never allocate from the low 1 MiB (IVT, BIOS data, VGA) ... */
    pmm_reserve(0, 0xA0000);
    pmm_reserve(0xC0000, 0x40000);
    /* ... or from the resident boot stub + kernel image. */
    pmm_reserve((uint64_t)__kernel_start,
                (uint64_t)__kernel_end - (uint64_t)__kernel_start);
}

void* pmm_alloc_pages(size_t count) {
    if (count == 0) count = 1;
    uint64_t run = 0;
    for (uint64_t f = 0; f < PMM_MAX_PAGES; f++) {
        if (pmm_is_free(f)) {
            run++;
            if (run == count) {
                uint64_t start = f + 1 - run;
                for (uint64_t i = start; i < start + count; i++) {
                    pmm_set(i);
                    pmm_free_frames--;
                }
                return (void*)(start << 12);
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

void pmm_free_pages(void* addr, size_t count) {
    uint64_t frame = (uint64_t)addr >> 12;
    for (uint64_t i = 0; i < count; i++) {
        if (frame + i < PMM_MAX_PAGES && !pmm_is_free(frame + i)) {
            pmm_clear(frame + i);
            pmm_free_frames++;
        }
    }
}

uint64_t pmm_total_mem(void) { return pmm_total_frames << 12; }
uint64_t pmm_free_mem(void)   { return pmm_free_frames << 12; }
uint32_t pmm_region_count_get(void) { return pmm_region_count; }

struct pmm_region pmm_region_get(uint32_t i) {
    struct pmm_region r = {0, 0, 0};
    if (i < pmm_region_count) r = pmm_regions[i];
    return r;
}