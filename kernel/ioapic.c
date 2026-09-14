/*
 * ARC OS - IOAPIC interrupt routing.
 *
 * Once the local APIC is on we stop relying on the legacy PIC/ExtINT path
 * and program the IOAPIC's redirection table: the ISA IRQ lines (and any
 * MADT interrupt-source overrides) are mapped onto IOAPIC pins, each with
 * a distinct vector delivered straight to the BSP LAPIC. The PIC is then
 * fully masked and LINT0 (ExtINT) disabled. If no IOAPIC is described we
 * leave the PIC/ExtINT fallback in place and just print a message.
 */

#include "kernel.h"

#define IOAPIC_REG_SEL     0x00
#define IOAPIC_REG_WIN     0x10
#define IOAPIC_VER         0x01
#define IOAPIC_REDTBL      0x10

#define IOAPIC_DEL_FIXED   0x0    /* delivery mode: fixed (vector)          */
#define IOAPIC_DEL_EXTINT  0x7

#define RTE_ACTIVE_LOW     (1ul << 13)
#define RTE_LEVEL          (1ul << 15)
#define RTE_MASKED         (1ul << 16)

#define IRQ_VEC_PIT    32
#define IRQ_VEC_KBD    33
#define IRQ_VEC_ATA    46

static volatile uint32_t* ioapic = 0;
static int ioapic_present = 0;

static void ioapic_write_reg(uint32_t reg, uint32_t val) {
    *ioapic = reg;
    *(ioapic + (IOAPIC_REG_WIN >> 2)) = val;
}

static uint32_t ioapic_read_reg(uint32_t reg) {
    *ioapic = reg;
    return *(ioapic + (IOAPIC_REG_WIN >> 2));
}

/* Route one redirection-table entry. dest = BSP LAPIC id (physical mode). */
static void ioapic_route(uint32_t gsi, uint8_t vec, uint32_t flags) {
    uint32_t lo = vec | (IOAPIC_DEL_FIXED << 8) | flags;
    uint32_t hi = 0;                       /* dest APIC id = 0 (BSP) */
    ioapic_write_reg(IOAPIC_REDTBL + 2 * gsi, lo);
    ioapic_write_reg(IOAPIC_REDTBL + 2 * gsi + 1, hi);
}

/* Route an ISA IRQ honoring any MADT interrupt-source override. */
static void ioapic_route_isa(int irq, uint8_t vec, int unmask) {
    int gsi, pol, trig;
    if (acpi_iso_get(irq, &gsi, &pol, &trig) < 0) {
        gsi = irq; pol = 0; trig = 0;
    }
    uint32_t flags = RTE_MASKED;
    if (pol)      flags |= RTE_ACTIVE_LOW;
    if (trig)     flags |= RTE_LEVEL;
    if (unmask)   flags &= ~RTE_MASKED;
    ioapic_route((uint32_t)gsi, vec, flags);
}

void ioapic_init(void) {
    uint32_t base = acpi_ioapic_base();
    if (!base) {
        terminal_putstring("IOAPIC: not in MADT, keeping PIC/ExtINT\n");
        return;
    }

    vmm_map_page(base, base, 0);
    ioapic = (volatile uint32_t*)base;

    uint32_t ver = ioapic_read_reg(IOAPIC_VER);
    uint32_t pins = ((ver >> 16) & 0xFF) + 1;

    /* Mask every pin first so nothing fires while we rewire. */
    for (uint32_t gsi = 0; gsi < pins; gsi++)
        ioapic_route(gsi, 0, RTE_MASKED);

    /* Wire the IRQs we use to the same vectors the PIC used to provide. */
    ioapic_route_isa(0,  IRQ_VEC_PIT, 0);   /* stays masked: LAPIC is the tick */
    ioapic_route_isa(1,  IRQ_VEC_KBD, 1);
    ioapic_route_isa(15, 0,          0);

    /* ATA primary channel. QEMU's MADT has no override for IRQ14, so the
     * generic path would program the pin as EDGE. QEMU's PIIX IDE drives it
     * as an active-high LEVEL: with edge semantics the line stays high, the
     * pin re-fires on every EOI, and IRQ14 storms. Force level-triggered. */
    {
        int g;
        if (acpi_iso_get(14, &g, 0, 0) < 0) g = 14;
        ioapic_route((uint32_t)g, IRQ_VEC_ATA, RTE_LEVEL);
    }

    /* Shut down the legacy PIC and its ExtINT path into the LAPIC. */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    lapic_mask_extint();

    ioapic_present = 1;
    terminal_putstring("IOAPIC @ 0x");
    terminal_print_hex(base);
    terminal_putstring(" ");
    terminal_print_int(pins);
    terminal_putstring(" pins, PIC disabled, IRQ0->GSI");
    int g;
    if (acpi_iso_get(0, &g, 0, 0) < 0) g = 0;
    terminal_print_int(g);
    terminal_putstring(" IRQ1->GSI1 IRQ14->GSI");
    if (acpi_iso_get(14, &g, 0, 0) < 0) g = 14;
    terminal_print_int(g);
    terminal_putstring("\n");
}

int ioapic_active(void) { return ioapic_present; }