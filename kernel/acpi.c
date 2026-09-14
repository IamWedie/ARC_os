/*
 * ARC OS - ACPI discovery (RSDP -> RSDT/XSDT -> MADT).
 * Tables live in reserved RAM that the identity map may or may not cover,
 * so each table's pages are mapped on demand before being read.
 */

#include "kernel.h"

#define ACPI_RSDP_SIG "RSD PTR "

static uint32_t acpi_lapic_addr = 0;
static uint32_t acpi_ioapic_addr = 0;
static int acpi_cpu_count = 0;
static uint8_t* acpi_madt = 0;

/* Interrupt-source overrides: IRQ -> GSI + polarity/trigger, filled from
 * MADT type-2 entries. Default when absent: IRQn routed to GSI n as an
 * active-high edge (which is what QEMU describes for the ISA lines). */
#define ACPI_ISO_MAX 16
static uint8_t acpi_iso_gsi[ACPI_ISO_MAX];
static uint8_t acpi_iso_pol[ACPI_ISO_MAX];
static uint8_t acpi_iso_trig[ACPI_ISO_MAX];

static uint8_t acpi_checksum(const uint8_t* p, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum;
}

/* Identity-map `len` bytes starting at physical `pa` (page granularity). */
static void acpi_map_phys(uintptr_t pa, uint32_t len) {
    for (uintptr_t a = pa & ~0xFFFULL; a < pa + len; a += 4096) {
        extern void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags);
        vmm_map_page(a, a, 0);
    }
}

static uint8_t* find_rsdp(void) {
    for (uintptr_t a = 0xE0000; a < 0x100000; a += 16) {
        acpi_map_phys(a, 16);
        const uint8_t* p = (const uint8_t*)a;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ' &&
            acpi_checksum(p, 20) == 0)
            return (uint8_t*)a;
    }
    /* EBDA pointer at 0x40E (word), 16-byte aligned KB address. */
    uint32_t ebda = *(volatile uint16_t*)0x40E << 4;
    for (uintptr_t a = ebda; a < ebda + 1024; a += 16) {
        acpi_map_phys(a, 16);
        const uint8_t* p = (const uint8_t*)a;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ' &&
            acpi_checksum(p, 20) == 0)
            return (uint8_t*)a;
    }
    return 0;
}

static uint8_t* find_table(uintptr_t pa, uint32_t len) {
    acpi_map_phys(pa, len);
    const uint8_t* h = (const uint8_t*)pa;
    uint32_t tlen = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);
    if (tlen < 36 || tlen > 0x100000) return 0;
    acpi_map_phys(pa, tlen);
    if (acpi_checksum((const uint8_t*)pa, (int)tlen) != 0) return 0;
    return (uint8_t*)pa;
}

void acpi_init(void) {
    uint8_t* r = find_rsdp();
    if (!r) {
        terminal_putstring("ACPI: no RSDP found\n");
        return;
    }
    for (int i = 0; i < ACPI_ISO_MAX; i++) {
        acpi_iso_gsi[i] = (uint8_t)i;
        acpi_iso_pol[i] = 0;
        acpi_iso_trig[i] = 0;
    }
    uint8_t rev = r[15];
    uint32_t rsdt = r[16] | (r[17] << 8) | (r[18] << 16) | (r[19] << 24);
    uint64_t xsdt = 0;
    if (rev >= 2) {
        for (int i = 0; i < 8; i++) xsdt |= (uint64_t)(r[24 + i]) << (8 * i);
    }

    terminal_putstring("ACPI: RSDP rev ");
    terminal_print_int(rev);
    terminal_putstring("\n");

    const int ptrsz = (rev >= 2 && xsdt) ? 8 : 4;
    uintptr_t root = (ptrsz == 8) ? (uintptr_t)xsdt : (uintptr_t)rsdt;

    uint8_t* tab = find_table(root, 0x1000);
    if (!tab) {
        terminal_putstring("ACPI: no root table\n");
        return;
    }
    uint32_t root_len = tab[4] | (tab[5] << 8) | (tab[6] << 16) | (tab[7] << 24);
    int n = (int)((root_len - 36) / ptrsz);

    for (int i = 0; i < n; i++) {
        uintptr_t pa = (ptrsz == 8) ? (uintptr_t)(*(uint64_t*)(tab + 36 + i * 8))
                                    : *(uint32_t*)(tab + 36 + i * 4);
        uint8_t* t = find_table(pa, 0x1000);
        if (!t) continue;
        if (t[0] == 'A' && t[1] == 'P' && t[2] == 'I' && t[3] == 'C') {
            acpi_madt = t;
            acpi_lapic_addr = *(uint32_t*)(t + 36);
            uint32_t madt_len = t[4] | (t[5] << 8) | (t[6] << 16) | (t[7] << 24);
            acpi_map_phys(pa, madt_len);

            uint32_t off = 44;
            int ioapics = 0;
            while (off + 2 <= madt_len) {
                uint8_t type = t[off];
                uint8_t len  = t[off + 1];
                if (len < 2) break;
                if (type == 0) {
                    acpi_cpu_count++;
                } else if (type == 1 && ioapics == 0) {
                    acpi_ioapic_addr = *(uint32_t*)(t + off + 4);
                    ioapics++;
                } else if (type == 5) {
                    /* LAPIC address override. */
                    acpi_lapic_addr = *(uint32_t*)(t + off + 4);
                } else if (type == 2) {
                    uint8_t src = t[off + 4];
                    uint8_t gsi = t[off + 6];
                    uint16_t fl = t[off + 7] | (t[off + 8] << 8);
                    if (src < ACPI_ISO_MAX) {
                        acpi_iso_gsi[src] = gsi;
                        acpi_iso_pol[src] = fl & 1;           /* 1 = low active */
                        acpi_iso_trig[src] = (fl >> 1) & 1;   /* 1 = level      */
                    }
                }
                off += len;
            }
            break;
        }
    }

    terminal_putstring("ACPI: ");
    if (acpi_lapic_addr) {
        terminal_putstring("LAPIC @ 0x");
        terminal_print_hex(acpi_lapic_addr);
        terminal_putstring(" IOAPIC @ 0x");
        terminal_print_hex(acpi_ioapic_addr);
        terminal_putstring(" cpus=");
        terminal_print_int(acpi_cpu_count);
        terminal_putstring("\n");
    } else {
        terminal_putstring("no MADT\n");
    }
}

uint32_t acpi_lapic_base(void) { return acpi_lapic_addr; }
uint32_t acpi_ioapic_base(void) { return acpi_ioapic_addr; }
int acpi_cpu_total(void) { return acpi_cpu_count; }

/* Look up the wired GSI (and polarity/trigger) for an ISA IRQ.
 * Returns 0 and fills the out-params, or -1 if the IRQ is out of range. */
int acpi_iso_get(int irq, int* gsi, int* pol, int* trig) {
    if (irq < 0 || irq >= ACPI_ISO_MAX || !acpi_madt) return -1;
    if (gsi)  *gsi  = acpi_iso_gsi[irq];
    if (pol)  *pol  = acpi_iso_pol[irq];
    if (trig) *trig = acpi_iso_trig[irq];
    return 0;
}