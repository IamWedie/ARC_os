/* Minimal PCI bus enumeration (M4). */
#include "kernel.h"

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static struct pci_device g_pci[PCI_MAX_DEVICES];
static int g_pci_count;

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFCu);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CFG_ADDR, pci_addr(bus, slot, func, off));
    return inl(PCI_CFG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    outl(PCI_CFG_ADDR, pci_addr(bus, slot, func, off));
    outl(PCI_CFG_DATA, val);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFFu);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFFu);
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func);

static void pci_scan_device(uint8_t bus, uint8_t slot) {
    uint16_t vendor = pci_read16(bus, slot, 0, 0);
    if (vendor == 0xFFFF) return;
    pci_scan_function(bus, slot, 0);
    uint8_t header = pci_read8(bus, slot, 0, 0x0E);
    if (header & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            if (pci_read16(bus, slot, func, 0) != 0xFFFF)
                pci_scan_function(bus, slot, func);
        }
    }
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (g_pci_count >= PCI_MAX_DEVICES) return;
    struct pci_device* d = &g_pci[g_pci_count];
    d->bus = bus; d->slot = slot; d->func = func;
    uint32_t id = pci_config_read32(bus, slot, func, 0x00);
    d->vendor = (uint16_t)(id & 0xFFFFu);
    d->device = (uint16_t)(id >> 16);
    if (d->vendor == 0xFFFF) return;
    uint32_t classrev = pci_config_read32(bus, slot, func, 0x08);
    d->revision = (uint8_t)(classrev & 0xFFu);
    d->progif = (uint8_t)((classrev >> 8) & 0xFFu);
    d->subclass = (uint8_t)((classrev >> 16) & 0xFFu);
    d->class_code = (uint8_t)((classrev >> 24) & 0xFFu);
    d->header_type = pci_read8(bus, slot, func, 0x0E) & 0x7Fu;
    uint32_t intr = pci_config_read32(bus, slot, func, 0x3C);
    d->irq_line = (uint8_t)(intr & 0xFFu);
    d->irq_pin = (uint8_t)((intr >> 8) & 0xFFu);
    for (int i = 0; i < 6; i++) d->bar[i] = 0;
    if (d->header_type == 0x00) {
        for (int i = 0; i < 6; i++) {
            d->bar[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
            if ((d->bar[i] & 0x6) == 0x4) i++; /* 64-bit BAR, skip high half in simple record */
        }
    }
    g_pci_count++;

    if (d->class_code == 0x06 && d->subclass == 0x04) {
        uint32_t buses = pci_config_read32(bus, slot, func, 0x18);
        uint8_t secondary = (uint8_t)((buses >> 8) & 0xFFu);
        if (secondary != 0) {
            for (uint8_t dev = 0; dev < 32; dev++) pci_scan_device(secondary, dev);
        }
    }
}

void pci_init(void) {
    g_pci_count = 0;
    uint8_t header = pci_read8(0, 0, 0, 0x0E);
    if ((header & 0x80) == 0) {
        for (uint8_t dev = 0; dev < 32; dev++) pci_scan_device(0, dev);
    } else {
        for (uint8_t func = 0; func < 8; func++) {
            if (pci_read16(0, 0, func, 0) == 0xFFFF) continue;
            uint8_t bus = func;
            for (uint8_t dev = 0; dev < 32; dev++) pci_scan_device(bus, dev);
        }
    }
    terminal_putstring("PCI: ");
    terminal_print_int((uint64_t)g_pci_count);
    terminal_putstring(" device(s)\n");
}

int pci_device_count(void) { return g_pci_count; }
const struct pci_device* pci_device_get(int i) {
    if (i < 0 || i >= g_pci_count) return 0;
    return &g_pci[i];
}
const struct pci_device* pci_find_class(uint8_t class_code, uint8_t subclass, int idx) {
    for (int i = 0; i < g_pci_count; i++) {
        if (g_pci[i].class_code == class_code && g_pci[i].subclass == subclass) {
            if (idx == 0) return &g_pci[i];
            idx--;
        }
    }
    return 0;
}