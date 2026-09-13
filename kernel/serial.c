/*
 * ARC OS - COM1 serial driver (16550 UART).
 * Used for kernel logs + interactive debug; terminal_putchar mirrors
 * everything to serial so QEMU -serial file captures full output.
 */
#include "kernel.h"

#define SERIAL_COM1 0x3F8

#define SERIAL_THR 0          /* transmit holding register */
#define SERIAL_RBR 0          /* receive buffer register   */
#define SERIAL_IER 1          /* interrupt enable register */
#define SERIAL_FCR 2          /* FIFO control register     */
#define SERIAL_LCR 3          /* line control register     */
#define SERIAL_MCR 4          /* modem control register    */
#define SERIAL_LSR 5          /* line status register      */

static inline void serial_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

int serial_init(void) {
    /* Disable interrupts. */
    serial_outb(SERIAL_COM1 + SERIAL_IER, 0x00);

    /* Set baud rate (divisor = 1 -> 115200 for a 1.8432 MHz clock). */
    serial_outb(SERIAL_COM1 + SERIAL_LCR, 0x80);  /* DLAB on */
    serial_outb(SERIAL_COM1 + SERIAL_THR, 0x01);  /* DLL low  */
    serial_outb(SERIAL_COM1 + SERIAL_IER, 0x00);  /* DLM high */
    serial_outb(SERIAL_COM1 + SERIAL_LCR, 0x03);  /* 8N1, DLAB off */

    /* Enable + clear FIFOs, 16-byte threshold. */
    serial_outb(SERIAL_COM1 + SERIAL_FCR, 0xC7);

    /* Loopback self-test: an echoed char should come back. */
    serial_outb(SERIAL_COM1 + SERIAL_MCR, 0x1E);  /* loopback on, DTR/RTS */
    serial_outb(SERIAL_COM1 + SERIAL_THR, 0xAE);
    uint8_t got = serial_inb(SERIAL_COM1 + SERIAL_RBR);
    serial_outb(SERIAL_COM1 + SERIAL_MCR, 0x0F);  /* loopback off */

    int ok = (got == 0xAE);
    if (ok) terminal_putstring("COM1 serial initialised (115200 8N1)\n");
    else    terminal_putstring("COM1 serial self-test FAILED\n");
    return ok;
}

static void serial_wait_tx(void) {
    for (int i = 0; i < 100000; i++) {
        if (serial_inb(SERIAL_COM1 + SERIAL_LSR) & 0x20) return;
    }
}

void serial_putchar(char c) {
    if (c == '\n') {
        serial_wait_tx();
        serial_outb(SERIAL_COM1 + SERIAL_THR, '\r');
    }
    serial_wait_tx();
    serial_outb(SERIAL_COM1 + SERIAL_THR, (uint8_t)c);
}

void serial_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) serial_putchar(s[i]);
}