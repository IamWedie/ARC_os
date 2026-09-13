#include "kernel.h"

uint8_t terminal_color = COLOR_LIGHT_GREY | (COLOR_BLUE << 4);
size_t terminal_row = 0;
size_t terminal_col = 0;
uint16_t* terminal_buffer = (uint16_t*)VGA_BUFFER;

void terminal_scroll(void) {
    for (size_t i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++) {
        terminal_buffer[i] = terminal_buffer[i + VGA_WIDTH];
    }
    for (size_t i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++) {
        terminal_buffer[i] = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
    }
    terminal_row = VGA_HEIGHT - 1;
    terminal_col = 0;
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_col = 0;
    terminal_clear();
}

void terminal_clear(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        terminal_buffer[i] = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
    }
}

void terminal_putchar(char c, uint8_t color) {
    serial_putchar(c);
    if (c == '\n') {
        terminal_row++;
        terminal_col = 0;
        if (terminal_row >= VGA_HEIGHT) terminal_scroll();
        return;
    }
    if (c == '\r') { terminal_col = 0; return; }
    terminal_buffer[terminal_row * VGA_WIDTH + terminal_col] = (uint16_t)c | ((uint16_t)color << 8);
    terminal_col++;
    if (terminal_col >= VGA_WIDTH) {
        terminal_col = 0;
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT) terminal_scroll();
    }
}

void terminal_putstring(const char* str) {
    while (*str) terminal_putchar(*str++, terminal_color);
}

void terminal_setcolor(uint8_t color) { terminal_color = color; }

static void print_uint(uint64_t n) {
    if (n == 0) { terminal_putchar('0', terminal_color); return; }
    char buf[20];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) terminal_putchar(buf[--i], terminal_color);
}

void terminal_print_int(uint64_t n) { print_uint(n); }

void terminal_print_hex(uint64_t n) {
    terminal_putstring("0x");
    for (int i = 28; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        terminal_putchar(d < 10 ? '0' + d : 'a' + d - 10, terminal_color);
    }
}
