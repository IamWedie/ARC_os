/*
 * ARC OS - Virtual memory manager (PAE, 2 paging levels used for now).
 *
 * A kernel page-table pool in .bss fully identity-maps usable RAM using
 * 2 MiB large pages, replacing boot32's hardcoded 512 MiB map. A 4 KiB
 * map/unmap facility exists for dynamic (future user/heap) mappings.
 */
#include "kernel.h"
#include "mboot.h"

#define PG_PRESENT (1ULL << 0)
#define PG_RW      (1ULL << 1)
#define PG_USER    (1ULL << 2)
#define PG_PSE     (1ULL << 7)

#define VMM_PD_COUNT 128            /* 128 * 1 GiB = 128 GiB of address space */

static uint64_t __attribute__((aligned(4096)))
    vmm_pml4[512];
static uint64_t __attribute__((aligned(4096)))
    vmm_pdpt[512];
static uint64_t __attribute__((aligned(4096)))
    vmm_pd[VMM_PD_COUNT][512];

static void vmm_invlpg(uint64_t vaddr) {
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

static void vmm_map_2mb(uint64_t vaddr, uint64_t paddr) {
    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    uint64_t pdi   = (vaddr >> 21) & 0x1FF;

    if (pml4i != 0 || pdpti >= VMM_PD_COUNT) return; /* below 128 GiB only */
    vmm_pd[pdpti][pdi] = (paddr & 0xFFE00000ULL) | PG_PRESENT | PG_RW | PG_PSE;
}

void vmm_init(void) {
    vmm_pml4[0] = (uint64_t)vmm_pdpt | PG_PRESENT | PG_RW;
    for (int i = 0; i < VMM_PD_COUNT; i++)
        vmm_pdpt[i] = (uint64_t)vmm_pd[i] | PG_PRESENT | PG_RW;

    /* First 2 MiB always present: low memory, BIOS/VGA, boot stub. */
    vmm_map_2mb(0, 0);

    /* Identity-map all physical regions referenced by the multiboot memory
     * map (both available and reserved: reserved regions hold ACPI tables,
     * e.g. QEMU parks them in the top 1 MiB of RAM which is reported as
     * reserved). Physical addresses are identity-mapped so any table at any
     * physical address is accessible. */
    uint32_t n = pmm_region_count_get();
    for (uint32_t i = 0; i < n; i++) {
        struct pmm_region r = pmm_region_get(i);
        uint64_t claimed_end = r.base + r.len;
        uint64_t start = (r.base + 0x1FFFFF) & ~0x1FFFFFULL; /* round up */
        /* Iterate over each 2 MiB page that overlaps the claimed range;
         * the last page may only partially overlap (region ends at a
         * non-2 MiB boundary), but identity-mapping the whole page of real
         * RAM is harmless and keeps tables near the top of RAM reachable. */
for (uint64_t a = start; a < claimed_end && a >= start; a += 0x200000) {
            uint64_t pdpti = (a >> 30) & 0x1FF;
            if (pdpti >= VMM_PD_COUNT) break;
            vmm_map_2mb(a, a);
        }
    }

    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)vmm_pml4) : "memory");
}

void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags) {
    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    uint64_t pdi   = (vaddr >> 21) & 0x1FF;
    uint64_t pti   = (vaddr >> 12) & 0x1FF;
    uint64_t fl    = PG_PRESENT | (flags ? flags : PG_RW);

    uint64_t* pml4 = (uint64_t*)vmm_pml4;
    if (!(pml4[pml4i] & PG_PRESENT)) {
        uint64_t t = (uint64_t)pmm_alloc_pages(1);
        memset((void*)t, 0, 4096);
        pml4[pml4i] = t | fl;
    }
    uint64_t* pdpt = (uint64_t*)(pml4[pml4i] & 0x000FFFFFFF000ULL);
    if (!(pdpt[pdpti] & PG_PRESENT)) {
        uint64_t t = (uint64_t)pmm_alloc_pages(1);
        memset((void*)t, 0, 4096);
        pdpt[pdpti] = t | fl;
    }
    if (pdpt[pdpti] & PG_PSE) return; /* already a 1 GiB page; not handled */
    uint64_t* pd = (uint64_t*)(pdpt[pdpti] & 0x000FFFFFFF000ULL);
    if (pd[pdi] & PG_PSE) return;     /* already a 2 MiB page; not handled */
    if (!(pd[pdi] & PG_PRESENT)) {
        uint64_t t = (uint64_t)pmm_alloc_pages(1);
        memset((void*)t, 0, 4096);
        pd[pdi] = t | fl;
    }
    uint64_t* pt = (uint64_t*)(pd[pdi] & 0x000FFFFFFF000ULL);
    pt[pti] = (paddr & ~0xFFFULL) | fl;
    vmm_invlpg(vaddr);
}

void vmm_unmap_page(uint64_t vaddr) {
    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    uint64_t pdi   = (vaddr >> 21) & 0x1FF;
    uint64_t pti   = (vaddr >> 12) & 0x1FF;

    uint64_t* pml4 = (uint64_t*)vmm_pml4;
    if (!pml4[pml4i]) return;
    uint64_t* pdpt = (uint64_t*)(pml4[pml4i] & 0x000FFFFFFF000ULL);
    if (!pdpt[pdpti]) return;
    uint64_t* pd = (uint64_t*)(pdpt[pdpti] & 0x000FFFFFFF000ULL);
    if (!pd[pdi]) return;
    uint64_t* pt = (uint64_t*)(pd[pdi] & 0x000FFFFFFF000ULL);
    pt[pti] = 0;
    vmm_invlpg(vaddr);
}

/* Map `pages` contiguous physical frames at vaddr as user-accessible pages. */
void vmm_create_user_mapping(uint64_t vaddr, uint64_t paddr, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(vaddr + i * 4096, paddr + i * 4096, PG_USER | PG_WRITABLE);
    }
}