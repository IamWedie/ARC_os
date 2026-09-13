#include "kernel.h"
#include <stdint.h>

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Timer interrupt handler (IRQ0, vector 32) */
void irq0_handler(void) {
    outb(0x20, 0x20);
    scheduler_yield();
}

/* Keyboard scancode to ASCII map */
static const char scancode_map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,0,0,
    0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0
};

/* Keyboard ring buffer (single producer = IRQ1, single consumer) */
#define KBD_BUFSIZE 256
static volatile uint8_t kbd_buffer[KBD_BUFSIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;

void irq1_handler(void) {
    uint8_t scancode = inb(0x60);
    if ((scancode & 0x80) == 0 && scancode < 128) {
        char c = scancode_map[scancode];
        if (c != 0) {
            int next = (kbd_head + 1) & (KBD_BUFSIZE - 1);
            if (next != kbd_tail) {
                kbd_buffer[kbd_head] = (uint8_t)c;
                kbd_head = next;
            }
        }
    }
    outb(0x20, 0x20);
}

int kbd_getchar(void) {
    if (kbd_head == kbd_tail) return -1;
    int c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) & (KBD_BUFSIZE - 1);
    return c;
}

void interrupts_init(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);

    idt_set_gate(32, (uint64_t)irq0_stub, 0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1_stub, 0x08, 0x8E);

    outb(0x20, 0x20);
    outb(0xA0, 0x20);

    __asm__ volatile("sti");

    outb(0x21, 0x01);
    outb(0xA1, 0x00);
}
