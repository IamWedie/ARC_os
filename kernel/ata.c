/*
 * ARC OS - ATA PIO driver (primary channel, 28-bit LBA).
 *
 * Sector I/O on the primary IDE controller using polled PIO with the
 * optional IRQ14 wake-up. WSyscalls run with the interrupt gate's IF off,
 * so the transfer loop never waits on IRQs alone: it always polls the
 * status register too, making the driver work whether or not the IRQ is
 * actually routed. The IRQ handler (ISR vector 46, via the IOAPIC) just
 * shortcuts the wait by raising a flag; reading the status/deassert path
 * is what lowers the level-triggered line in QEMU.
 */

#include "kernel.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_NSECT       0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_CMD         0x1F7
#define ATA_ALT_STATUS  0x3F6

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30

#define ATA_WAIT_MAX 40000000UL

static int ata_ok = 0;
static volatile int ata_irq_hit = 0;

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

void ata_irq_handler(void) {
    ata_irq_hit = 1;
    (void)inb(ATA_CMD);            /* STATUS read deasserts INTRQ in QEMU  */
    outb(0x20, 0x20);              /* PIC EOI: harmless if the PIC is off  */
    outb(0xA0, 0x20);              /* slave PIC EOI too (IRQ14 lives there)*/
    lapic_eoi();
}

static uint8_t ata_status(void) { return inb(ATA_CMD); }

/* Wait for the controller to be idle (BSY clear). Returns status or 0xFF. */
static uint8_t ata_wait_bsy(void) {
    for (uint32_t i = 0; i < ATA_WAIT_MAX; i++) {
        uint8_t st = ata_status();
        if (!(st & ATA_SR_BSY)) return st;
        __asm__ volatile("pause");
    }
    return 0xFF;
}

/* Wait for BSY clear + the data-port to be ready, or for the IRQ flag.
 * Also bails on the error bits so a dead/no device returns quickly-ish. */
static uint8_t ata_wait_ready(void) {
    uint32_t idle = 0;
    for (uint32_t i = 0; i < ATA_WAIT_MAX; i++) {
        if (ata_irq_hit) {           /* IRQ seen: consume and report the bus */
            ata_irq_hit = 0;
            return ata_status();
        }
        uint8_t st = ata_status();
        if (st & ATA_SR_ERR) return st;
        if (st == 0xFF) {            /* nothing on the bus at all */
            if (++idle > 1000) return 0xFF;
        }
        if (!(st & ATA_SR_BSY)) return st;
        __asm__ volatile("pause");
    }
    return 0xFF;
}

int ata_present(void) { return ata_ok; }

int ata_read_sectors(uint32_t lba, uint8_t count, void* buf) {
    if (!ata_ok || count == 0) return -1;
    preempt_disable();
    ata_irq_hit = 0;

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_NSECT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_CMD, ATA_CMD_READ);

    uint16_t* w = (uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint8_t st = ata_wait_ready();
        if (st & (ATA_SR_ERR | ATA_SR_DF) || st == 0xFF) {
            preempt_enable();
            return -1;
        }
        for (int i = 0; i < 256; i++) w[s * 256 + i] = inw(ATA_DATA);
    }
    preempt_enable();
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf) {
    if (!ata_ok || count == 0) return -1;
    preempt_disable();
    ata_irq_hit = 0;

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_NSECT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_CMD, ATA_CMD_WRITE);

    const uint16_t* w = (const uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint8_t st = ata_wait_ready();
        if (st & (ATA_SR_ERR | ATA_SR_DF) || st == 0xFF) {
            preempt_enable();
            return -1;
        }
        for (int i = 0; i < 256; i++) outw(ATA_DATA, w[s * 256 + i]);
        /* Last sector: wait for the device to absorb the write. */
        if (s + 1 == count) {
            uint8_t st2 = ata_wait_bsy();
            if (st2 & (ATA_SR_ERR | ATA_SR_DF) || st2 == 0xFF) {
                preempt_enable();
                return -1;
            }
        }
    }
    preempt_enable();
    return 0;
}

int ata_init(void) {
    ata_irq_hit = 0;
    outb(ATA_DRIVE, 0xE0);                /* select primary master */
    if (ata_wait_bsy() == 0xFF) {
        terminal_putstring("ATA: no drive on primary master\n");
        return -1;
    }

    outb(ATA_NSECT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    uint8_t st = ata_wait_ready();
    if (st & (ATA_SR_ERR | ATA_SR_DF) || st == 0xFF) {
        terminal_putstring("ATA: IDENTIFY failed\n");
        return -1;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    if (id[0] == 0x0000 || id[0] == 0xFFFF) {
        terminal_putstring("ATA: empty IDENTIFY block\n");
        return -1;
    }

    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    ata_ok = 1;
    terminal_putstring("ATA: drive detected, ");
    terminal_print_int(sectors);
    terminal_putstring(" sectors\n");
    return 0;
}