/*
 * ARC OS - Local APIC + LAPIC timer.
 * The LAPIC is switched on via its MSR, mapped at its MMIO base, and its
 * timer is calibrated against one PIT (IRQ0) period to provide the 100 Hz
 * preemption tick; the PIT's IRQ0 is then masked in the PIC.
 */

#include "kernel.h"

#define MSR_APIC_BASE  0x1B
#define APIC_ENABLE    (1ULL << 11)
#define APIC_BSP       (1ULL << 8)

#define LAPIC_ID       0x020
#define LAPIC_SVR      0x0F0
#define LAPIC_EOI      0x0B0
#define LAPIC_ICRLO    0x300
#define LAPIC_ICRHI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_TIMER_DIV 0x3E0
#define LAPIC_TIMER_INITCNT 0x380
#define LAPIC_TIMER_CURCNT  0x390

#define LAPIC_TIMER_VECTOR 0x40

static volatile uint32_t* lapic = 0;
volatile int pit_tick_count = 0;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

static uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

void lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

/* Preemption handler: LAPIC timer tick (vector 0x40). */
uint64_t apic_timer_handler(uint64_t rsp) {
    lapic_eoi();
    return scheduler_switch(rsp);
}

uint32_t acpi_lapic_base(void);

void lapic_init(void) {
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (!(msr & APIC_BSP)) {
        terminal_putstring("LAPIC: not on BSP, skipping\n");
        return;
    }
    msr |= APIC_ENABLE;
    wrmsr(MSR_APIC_BASE, msr);

    uintptr_t base = acpi_lapic_base();
    if (!base) base = (uintptr_t)((msr >> 12) & 0xFFFFF) << 12;
    if (!base) {
        terminal_putstring("LAPIC: no base address\n");
        return;
    }

    vmm_map_page(base, base, 0);
    lapic = (volatile uint32_t*)base;

    /* Enable the local APIC (bit 8) with a spurious vector. */
    lapic_write(LAPIC_SVR, 0x100 | 0xFF);
    /* Route legacy PIC IRQs (keyboard etc.) through LINT0 as ExtINT so they
     * are still delivered now that the local APIC is on. */
    lapic_write(LAPIC_LVT_LINT0, 0x700);
    /* Mask the timer until calibrated. */
    lapic_write(LAPIC_LVT_TIMER, 0x10000);

    terminal_putstring("LAPIC enabled @ 0x");
    terminal_print_hex(base);
    terminal_putstring(" id=");
    terminal_print_int((lapic_read(LAPIC_ID) >> 24) & 0xFF);
    terminal_putstring("\n");
}

/* Latch channel 0's current count. */
static uint32_t pit_read_count(void) {
    outb(0x43, 0x00);
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (uint32_t)(lo | (hi << 8));
}

/* Calibrate the LAPIC timer against a ~10 ms one-shot on PIT channel 0.
 * The PIT counter is polled directly; the legacy PIC IRQ0 cannot be used
 * for this because the PIT tick is delivered through the (masked) LAPIC
 * LINT0 once the local APIC is enabled. */
static uint32_t lapic_calibrate(void) {
    /* One-shot ~10 ms on channel 0: 1193182 Hz -> 11932 counts. */
    outb(0x43, 0x30);              /* ch0, mode 0, lobyte then hibyte */
    outb(0x40, 11932 & 0xFF);
    outb(0x40, (11932 >> 8) & 0xFF);

    lapic_write(LAPIC_TIMER_DIV, 0x3);              /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, 0x10000);          /* one-shot, masked */
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    int guard = 0;
    while (pit_read_count() != 0) {
        __asm__ volatile("pause");
        if (guard > 100000000) break;
        guard++;
    }

    uint32_t cur = lapic_read(LAPIC_TIMER_CURCNT);
    uint32_t elapsed = 0xFFFFFFFF - cur;
    if (elapsed == 0) elapsed = 1;
    /* elapsed counts elapsed in ~10 ms = one 100 Hz tick. */
    return elapsed;
}

void lapic_timer_start(void) {
    uint32_t count = lapic_calibrate();
    lapic_write(LAPIC_TIMER_DIV, 0x3);              /* divide by 16 */
    /* periodic (bit 17), unmasked, vector 0x40 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | 0x20000);
    lapic_write(LAPIC_TIMER_INITCNT, count ? count : 0x1000);
    terminal_putstring("LAPIC timer arm: ");
    terminal_print_int(count);
    terminal_putstring(" counts/100Hz\n");
}