#include "kernel.h"

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
__attribute__((weak)) int fat32_is_mounted(void) { return 0; }
__attribute__((weak)) int vfs_stat(const char* path, struct arc_stat* st) { (void)path; (void)st; return -1; }
__attribute__((weak)) int vfs_readdir(const char* path, uint32_t index, struct arc_dirent* out) { (void)path; (void)index; (void)out; return -1; }
__attribute__((weak)) int vfs_mkdir(const char* path) { (void)path; return -1; }
__attribute__((weak)) int vfs_unlink(const char* path) { (void)path; return -1; }
__attribute__((weak)) int vfs_mount_device(const char* devname, const char* fstype, const char* target) { (void)devname; (void)fstype; (void)target; return -1; }
__attribute__((weak)) int fs_readdir(const char* path, uint32_t index, struct arc_dirent* out) { (void)path; (void)index; (void)out; return -1; }
#endif

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
    case SYS_WRITE: {
        size_t count = (size_t)a3;
        uint64_t va = a2;
        /* A zero-length write must not dereference the user pointer. */
        if (count == 0) return 0;
        if (count > UINT64_MAX - va) return -1;
        if (user_range_readable((const void*)(uintptr_t)va, count) != 0)
            return -1;

        uint8_t bounce[512];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
            if (copy_from_user(bounce,
                               (const void*)(uintptr_t)(va + done),
                               chunk) != 0) return -1;

            int wrote = fd_write(pid, (int)a1, bounce, chunk);
            if (wrote < 0) return -1;
            done += (size_t)wrote;
            if ((size_t)wrote < chunk) break;
        }
        return (long)done;
    }
    case SYS_READ: {
        int fd = (int)a1;
        size_t count = (size_t)a3;
        uint64_t va = a2;
        /* A zero-length read must never block, even on console stdin. */
        if (count == 0) return 0;
        if (count > UINT64_MAX - va) return -1;
        if (user_range_writable((void*)(uintptr_t)va, count) != 0)
            return -1;

        uint8_t bounce[512];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(bounce)) chunk = sizeof(bounce);

            int ret = fd_read(pid, fd, bounce, chunk);
            /*
             * fd_read() stays non-blocking so the hosted tests can link fd.c
             * directly. Console stdin is the one blocking stream: at the
             * kernel syscall boundary, park with sti; hlt until a byte is
             * available. Only a real console-in descriptor retries -- an
             * empty regular file (including a closed-and-reused fd 0)
             * returns 0 immediately.
             */
            if (ret == 0 && fd_is_console_in(pid, fd)) {
                do {
                    __asm__ volatile("sti; hlt");
                    ret = fd_read(pid, fd, bounce, chunk);
                } while (ret == 0 && fd_is_console_in(pid, fd));
            }
            if (ret < 0) return -1;
            if (ret == 0) break;

            if (copy_to_user((void*)(uintptr_t)(va + done), bounce,
                             (size_t)ret) != 0) return -1;
            done += (size_t)ret;
            if ((size_t)ret < chunk) break;
        }
        return (long)done;
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
    case SYS_STAT: {
        struct arc_stat st;
        const char* path = (const char*)a1;
        struct arc_stat* out = (struct arc_stat*)a2;
        if (!path || !out) return -1;
        if (fat32_is_mounted()) {
            if (vfs_stat(path, &st) != 0) return -1;
        } else {
            const char* name = path;
            if (name[0] == '/' && name[1] != '\0') name++;
            if (name[0] == '/' && name[1] == '\0') { st.size=0; st.is_dir=1; st.attrs=0; st.reserved=0; *out=st; return 0; }
            int slot = fs_open_slot(name, 0);
            if (slot < 0) return -1;
            st.size = (uint32_t)fs_slot_size(slot);
            st.is_dir = 0;
            st.attrs = 0;
            st.reserved = 0;
        }
        *out = st;
        return 0;
    }
    case SYS_READDIR: {
        const char* path = (const char*)a1;
        uint32_t idx = (uint32_t)a2;
        struct arc_dirent* out = (struct arc_dirent*)a3;
        if (!path || !out) return -1;
        return fs_readdir(path, idx, out);
    }
    case SYS_MKDIR:
        if (!a1) return -1;
        if (fat32_is_mounted()) return vfs_mkdir((const char*)a1);
        return -1;
    case SYS_UNLINK:
        if (!a1) return -1;
        if (fat32_is_mounted()) return vfs_unlink((const char*)a1);
        {
            const char* name = (const char*)a1;
            if (name[0] == '/' && name[1] != '\0') name++;
            return fs_delete(name);
        }
    case SYS_MOUNT:
        if (!a1 || !a2 || !a3) return -1;
        return vfs_mount_device((const char*)a1, (const char*)a2, (const char*)a3);
    case SYS_SYNC:
        return vfs_sync_all();
    case SYS_FSYNC:
        return fd_fsync(pid, (int)a1);
    default:        return -1;
    }
}
