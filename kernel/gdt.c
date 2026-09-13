#include "kernel.h"
#include <stdint.h>

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    uint64_t base;
} __attribute__((packed));

/* 64-bit TSS (used for the ring-0 stack switch on privilege transitions). */
struct tss_t {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define GDT_SEL_KC 0x08   /* kernel code (ring 0) */
#define GDT_SEL_KD 0x10   /* kernel data (ring 0) */
#define GDT_SEL_UD 0x18   /* user data   (ring 3) */
#define GDT_SEL_UC 0x1B   /* user code   (ring 3) */
#define GDT_SEL_TSS 0x28  /* TSS (ring 0) */

#define GDT_ENTRIES 7

struct gdt_entry gdt[GDT_ENTRIES];
struct gdt_ptr gdt_ptr_data;
struct tss_t tss;

void set_tss_rsp0(uint64_t rsp) {
    tss.rsp0 = rsp;
}

static void gdt_set_tss(uint64_t base) {
    uint32_t lim = sizeof(struct tss_t) - 1;

    gdt[5].limit_low  = lim & 0xFFFF;
    gdt[5].base_low   = base & 0xFFFF;
    gdt[5].base_middle = (base >> 16) & 0xFF;
    gdt[5].access     = 0x89;                /* present, DPL0, 64-bit TSS */
    gdt[5].granularity = ((base >> 24) & 0x0F);
    gdt[5].base_high  = (base >> 24) & 0xFF;

    /* upper 4 bytes of the 64-bit TSS base */
    gdt[6].limit_low  = 0;
    gdt[6].base_low   = (base >> 32) & 0xFFFF;
    gdt[6].base_middle = (base >> 40) & 0xFF;
    gdt[6].access     = 0;
    gdt[6].granularity = 0;
    gdt[6].base_high  = 0;
}

void gdt_init(void) {
    gdt[0] = (struct gdt_entry){0, 0, 0, 0, 0, 0};
    /* kernel ring-0 code: L=1 in granularity byte */
    gdt[1] = (struct gdt_entry){0xFFFF, 0, 0, 0x9A, 0x20, 0};
    gdt[2] = (struct gdt_entry){0xFFFF, 0, 0, 0x92, 0xCF, 0};
    /* user ring-3 segments */
    gdt[3] = (struct gdt_entry){0xFFFF, 0, 0, 0xF2, 0xCF, 0};  /* data */
    gdt[4] = (struct gdt_entry){0xFFFF, 0, 0, 0xFA, 0x20, 0};  /* code, L=1 */

    gdt_ptr_data.limit = sizeof(gdt) - 1;
    gdt_ptr_data.base = (uint64_t)&gdt;

    gdt_set_tss((uint64_t)&tss);

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "movq $16, %%rax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "movq $40, %%rax\n\t"          /* TSS selector 0x28 */
        "ltr %%ax\n\t"
        :
        : "r"(&gdt_ptr_data)
        : "rax", "memory"
    );
}