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

/* Table pages come from the PMM; the walk core expects them pre-zeroed. */
static void* vmm_alloc_table(void) {
    void* page = pmm_alloc_pages(1);
    if (page) memset(page, 0, 4096);
    return page;
}

static void vmm_free_table(void* page) {
    pmm_free_pages(page, 1);
}

static const pt_ops_t vmm_ops = {
    .alloc = vmm_alloc_table,
    .free  = vmm_free_table,
};

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
        uint64_t start = r.base & ~0x1FFFFFULL; /* round down */
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
    pt_map(vmm_pml4, vaddr, paddr, flags, &vmm_ops);
    vmm_invlpg(vaddr);
}

void vmm_unmap_page(uint64_t vaddr) {
    pt_unmap(vmm_pml4, vaddr);
    vmm_invlpg(vaddr);
}

/* The master root is statically linked at its physical address (identity map). */
uint64_t vmm_master_root(void) {
    return (uint64_t)vmm_pml4;
}

void vmm_invalidate_page(uint64_t vaddr) {
    vmm_invlpg(vaddr);
}

void vmm_switch_address_space(int pid) {
    uint64_t root = vmm_master_root();
    if (pid > 0 && pid < MAX_PROCESSES && processes[pid].pml4_phys)
        root = processes[pid].pml4_phys;
    __asm__ volatile("movq %0, %%cr3" :: "r"(root) : "memory");
}