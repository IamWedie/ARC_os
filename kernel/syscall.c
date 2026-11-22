#include "kernel.h"

/* Flags accepted by SYS_OPEN. O_ACCMODE covers O_RDONLY/O_WRONLY/O_RDWR. */
#define SYSCALL_OPEN_FLAG_MASK \
    ((uint64_t)(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND))

void syscall_exit(int code) {
    terminal_putstring("[Process exited]\n");
    scheduler_exit_current(code);
}

/* Entry for the int 0x80 gate. Args arrive in SysV order:
 *   rdi = syscall number, rsi/rdx/rcx = a1/a2/a3 (set by user wrapper). */
long syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int pid = current_process;

    switch (n) {
    case SYS_WRITE:
        return fd_write(pid, (int)a1, (const void*)a2, (size_t)a3);
    case SYS_READ: {
        int fd = (int)a1;
        size_t count = (size_t)a3;
        /* A zero-length read must never block, even on console stdin. */
        if (count == 0) return 0;
        long ret = fd_read(pid, fd, (void*)a2, count);
        /*
         * fd_read() stays non-blocking so the hosted tests can link fd.c
         * directly. Console stdin is the one blocking stream: at the kernel
         * syscall boundary, park with sti; hlt until a byte is available.
         * Only a real console-in descriptor retries -- an empty regular file
         * (including a closed-and-reused fd 0) returns 0 immediately.
         */
        if (ret == 0 && fd_is_console_in(pid, fd)) {
            do {
                __asm__ volatile("sti; hlt");
                ret = fd_read(pid, fd, (void*)a2, count);
            } while (ret == 0 && fd_is_console_in(pid, fd));
        }
        return ret;
    }
    case SYS_EXIT:  syscall_exit((int)a1); return 0;
    case SYS_OPEN:
        /* a2 is the flag word: reject unknown bits and an invalid access mode
         * before the fd layer allocates anything. */
        if ((a2 & ~SYSCALL_OPEN_FLAG_MASK) != 0) return -1;
        if ((a2 & O_ACCMODE) == O_ACCMODE) return -1;
        return fd_open(pid, (const char*)a1, (uint32_t)a2);
    case SYS_CLOSE: return fd_close(pid, (int)a1);
    case SYS_LS:    fs_list(); return 0;
    case SYS_DEL:   return fs_delete((const char*)a1);
    case SYS_SEEK:
        if (a3 != SEEK_SET && a3 != SEEK_CUR && a3 != SEEK_END) return -1;
        return fd_seek(pid, (int)a1, (int64_t)a2, (int)a3);
    case SYS_SIZE:  return fd_size(pid, (int)a1);
    case SYS_TRUNC: return fd_truncate(pid, (int)a1);
    case SYS_DUP:   return fd_dup(pid, (int)a1);
    case SYS_RENAME:return fs_rename((const char*)a1, (const char*)a2);
    case SYS_CLEAR: terminal_clear(); return 0;
    case SYS_FORK:  return sys_fork();
    case SYS_WAIT:  return sys_wait((int*)a1);
    case SYS_EXEC:  return sys_exec((const char*)a1, (char* const*)a2);
    default:        return -1;
    }
}
