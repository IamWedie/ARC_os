/*
 * ARC OS - 8253/8254 PIT driver.
 * Channel 0 drives the preemption ticker at a configurable rate.
 */
#include "kernel.h"

#define PIT_BASE_HZ 1193182

void pit_init_freq(uint32_t hz) {
    if (hz == 0 || hz > 100000) hz = 100;
    uint32_t div = PIT_BASE_HZ / hz;
    if (div == 0) div = 1;
    /* channel 0, lobyte then hibyte, mode 3 (square wave) */
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
}

void pit_init(void) {
    pit_init_freq(100);
}