#include "kernel.h"

int syscall_write(int fd, const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        terminal_putchar(buf[i], terminal_color);
    }
    return count;
}

int syscall_read(int fd, char* buf, size_t count) {
    return 0;
}

void syscall_exit(int code) {
    terminal_putstring("[Process exited]\n");
}
