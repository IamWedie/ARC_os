#include "kernel.h"
#include <stdint.h>
#define IDT_SIZE 256

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char ist;
    unsigned char flags;
    unsigned short offset_mid;
    unsigned int offset_high;
    unsigned int reserved;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned long base;
} __attribute__((packed));

extern void idt_flush();

static struct idt_entry idt[IDT_SIZE];
static struct idt_ptr idt_ptr_data;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void idt_init(void) {
    idt_ptr_data.limit = sizeof(idt) - 1;
    idt_ptr_data.base = (unsigned long)&idt;
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, 0, 0x08, 0x8E);

    __asm__ volatile("lidt %0" : : "m"(idt_ptr_data));
}
