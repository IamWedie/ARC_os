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
    uint64_t base;
} __attribute__((packed));

extern void idt_flush();

static struct idt_entry idt[IDT_SIZE];
static struct idt_ptr idt_ptr_data;

static void idt_set_gate_common(uint8_t num, uint64_t base, uint16_t sel,
                                uint8_t flags, uint8_t ist) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = ist & 0x7;  /* IDT IST field is three bits */
    idt[num].flags = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_set_gate_common(num, base, sel, flags, 0);
}

void idt_set_exception_gate(uint8_t num, uint64_t base) {
    uint8_t ist = num == DOUBLE_FAULT_VECTOR ? DOUBLE_FAULT_IST_INDEX : 0;
    idt_set_gate_common(num, base, GDTSEL_KC, 0x8E, ist);
}

uint8_t idt_gate_ist(uint8_t num) {
    return idt[num].ist;
}

uint16_t idt_gate_selector(uint8_t num) {
    return idt[num].selector;
}

uint8_t idt_gate_flags(uint8_t num) {
    return idt[num].flags;
}

uint64_t idt_gate_offset(uint8_t num) {
    return ((uint64_t)idt[num].offset_high << 32) |
           ((uint64_t)idt[num].offset_mid << 16) |
           (uint64_t)idt[num].offset_low;
}

void idt_init(void) {
    idt_ptr_data.limit = sizeof(idt) - 1;
    idt_ptr_data.base = (uint64_t)(uintptr_t)&idt;
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, 0, 0x08, 0x8E);

    __asm__ volatile("lidt %0" : : "m"(idt_ptr_data));
}
