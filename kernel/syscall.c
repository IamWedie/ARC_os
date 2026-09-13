#include "kernel.h"

int syscall_write(int fd, const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        terminal_putchar(buf[i], terminal_color);
    }
    return count;
}

int syscall_read(int fd, char* buf, size_t count) {
    if (fd >= FS_SLOT_FIRST) return fs_read(fd, buf, count);
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

/* Entry for the int 0x80 gate. Args arrive in SysV order:
 *   rdi = syscall number, rsi/rdx/rcx = a1/a2/a3 (set by user wrapper). */
long syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (n) {
    case SYS_WRITE:
        if (a1 < FS_SLOT_FIRST)
            return syscall_write((int)a1, (const char*)a2, (size_t)a3);
        return fs_write((int)a1, (const void*)a2, (size_t)a3);
    case SYS_READ:
        return syscall_read((int)a1, (char*)a2, (size_t)a3);
    case SYS_EXIT:  syscall_exit((int)a1); return 0;
    case SYS_OPEN:  return fs_open((const char*)a1, (int)a2);
    case SYS_CLOSE: return fs_close((int)a1);
    case SYS_LS:    fs_list(); return 0;
    case SYS_DEL:   return fs_delete((const char*)a1);
    default:        return -1;
    }
}
