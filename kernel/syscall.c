#include "kernel.h"

int syscall_write(int fd, const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        terminal_putchar(buf[i], terminal_color);
    }
    return count;
}

int syscall_read(int fd, char* buf, size_t count) {
    size_t got = 0;
    while (got < count) {
        int c = kbd_getchar();
        while (c < 0) {
            __asm__ volatile("sti; hlt");
            c = kbd_getchar();
        }
        buf[got++] = (char)c;
        if (c == '\n' && fd == 0) break;
    }
    return got;
}

void syscall_exit(int code) {
    terminal_putstring("[Process exited]\n");
}
