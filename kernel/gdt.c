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

struct gdt_entry gdt[3];
struct gdt_ptr gdt_ptr_data;

void gdt_init(void) {
    gdt[0] = (struct gdt_entry){0,0,0,0,0,0};
    /* 64-bit code segment: L=1 in the granularity byte (0x20). The
       previous 0xCF (D=1, L=0) described a 32-bit code segment, which
       made interrupt-gate CS reloads fault with #GP and a triple fault. */
    gdt[1] = (struct gdt_entry){0xFFFF,0,0,0x9A,0x20,0};
    gdt[2] = (struct gdt_entry){0xFFFF,0,0,0x92,0xCF,0};
    gdt_ptr_data.limit = sizeof(gdt) - 1;
    gdt_ptr_data.base = (uint64_t)&gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "movq $16, %%rax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : "r"(&gdt_ptr_data)
        : "rax", "memory"
    );
}
