/*
 * ARC OS - per-process file descriptors.
 *
 * Each process owns a fixed descriptor table. A descriptor points either at
 * the console (stdin/stdout/stderr) or, once the filesystem slot backend is
 * wired up, at an entry in the shared open file description pool. No function
 * here allocates memory: both tables have fixed bounds.
 */
#include "kernel.h"

/* Shared open file descriptions. File-backed fds are added with the slot API. */
static open_file_t g_ofds[OFD_MAX] __attribute__((unused));

static fd_entry_t* fd_lookup(int pid, int fd) {
    if (pid < 0 || pid >= MAX_PROCESSES) return NULL;
    if (fd < 0 || fd >= FD_MAX) return NULL;
    return &processes[pid].fds[fd];
}

void fd_init_process(process_t* p) {
    if (!p) return;

    for (int fd = 0; fd < FD_MAX; fd++) {
        p->fds[fd].kind = FD_ENTRY_FREE;
        p->fds[fd].ofd = -1;
        p->fds[fd].flags = 0;
    }

    p->fds[FD_STDIN].kind = FD_ENTRY_CONSOLE_IN;
    p->fds[FD_STDOUT].kind = FD_ENTRY_CONSOLE_OUT;
    p->fds[FD_STDERR].kind = FD_ENTRY_CONSOLE_ERR;
}

int fd_close(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind == FD_ENTRY_FREE) return -1;

    /*
     * Console entries have no shared description to release. File entries
     * will drop the OFD reference here once fd_open exists.
     */
    entry->kind = FD_ENTRY_FREE;
    entry->ofd = -1;
    entry->flags = 0;
    return 0;
}

int fd_read(int pid, int fd, void* buf, size_t count) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || (!buf && count > 0)) return -1;
    if (entry->kind != FD_ENTRY_CONSOLE_IN) return -1;

    size_t n = 0;
    while (n < count) {
        int c = kbd_getchar();
        if (c < 0) break;
        ((char*)buf)[n++] = (char)c;
        if (c == '\n') break;
    }
    return (int)n;
}

int fd_write(int pid, int fd, const void* buf, size_t count) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || (!buf && count > 0)) return -1;

    switch (entry->kind) {
    case FD_ENTRY_CONSOLE_OUT:
    case FD_ENTRY_CONSOLE_ERR:
        terminal_write((const char*)buf, count);
        return (int)count;
    default:
        return -1;
    }
}

int fd_seek(int pid, int fd, int64_t offset, int whence) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    (void)offset;
    (void)whence;
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    /* File-backed OFDs are implemented with the filesystem slot API. */
    return -1;
}

int fd_size(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    return -1;
}

int fd_truncate(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    return -1;
}
