/*
 * ARC OS - per-process file descriptors.
 *
 * Each process owns a fixed descriptor table. A descriptor points either at
 * the console (stdin/stdout/stderr) or at an entry in the shared open file
 * description pool. An OFD holds the file slot, the shared offset, the open
 * flags, and a reference count; fds in one or more processes can point at the
 * same OFD.
 *
 * Slot references follow fds, not OFDs: fs_open_slot() resolves or creates the
 * file without taking a reference, every installed descriptor calls
 * fs_slot_add_ref() once, and closing a descriptor calls fs_slot_release()
 * once. That keeps the backend's open_refs equal to the number of open
 * descriptors, which is exactly what deferred unlink needs.
 *
 * No function here allocates memory: both tables have fixed bounds.
 */
#include "kernel.h"

/* Shared open file descriptions. */
static open_file_t g_ofds[OFD_MAX];

/* Flags accepted by fd_open(). O_ACCMODE covers O_RDONLY/O_WRONLY/O_RDWR. */
#define FD_OPEN_FLAG_MASK \
    ((uint32_t)(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND))

static fd_entry_t* fd_lookup(int pid, int fd) {
    if (pid < 0 || pid >= MAX_PROCESSES) return NULL;
    if (fd < 0 || fd >= FD_MAX) return NULL;
    return &processes[pid].fds[fd];
}

static open_file_t* fd_entry_ofd(const fd_entry_t* entry) {
    if (entry->kind != FD_ENTRY_FILE) return NULL;
    if (entry->ofd < 0 || entry->ofd >= OFD_MAX) return NULL;
    if (!g_ofds[entry->ofd].used) return NULL;
    return &g_ofds[entry->ofd];
}

/*
 * Drop the slot reference held by one descriptor and clear the entry. The
 * final reference also retires the OFD so its index can be reused.
 */
static void fd_release_entry(fd_entry_t* entry) {
    open_file_t* ofd = fd_entry_ofd(entry);
    if (ofd) {
        fs_slot_release(ofd->file_slot);
        if (ofd->refs > 0) ofd->refs--;
        if (ofd->refs == 0) {
            ofd->used = 0;
            ofd->file_slot = -1;
            ofd->offset = 0;
            ofd->flags = 0;
        }
    }

    entry->kind = FD_ENTRY_FREE;
    entry->ofd = -1;
    entry->flags = 0;
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

int fd_open(int pid, const char* name, uint32_t flags) {
    if (!name) return -1;
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if ((flags & O_ACCMODE) == O_ACCMODE) return -1;
    if ((flags & ~FD_OPEN_FLAG_MASK) != 0) return -1;

    /* Reserve the descriptor and OFD before touching the filesystem. */
    int fd = -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (processes[pid].fds[i].kind == FD_ENTRY_FREE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    int ofd_index = -1;
    for (int i = 0; i < OFD_MAX; i++) {
        if (!g_ofds[i].used) {
            ofd_index = i;
            break;
        }
    }
    if (ofd_index < 0) return -1;

    int slot = fs_open_slot(name, (flags & O_CREAT) != 0);
    if (slot < 0) return -1;
    if (fs_slot_add_ref(slot) < 0) return -1;

    if ((flags & O_TRUNC) && fs_slot_truncate(slot) < 0) {
        fs_slot_release(slot);
        return -1;
    }

    g_ofds[ofd_index].used = 1;
    g_ofds[ofd_index].file_slot = slot;
    g_ofds[ofd_index].offset = 0;
    g_ofds[ofd_index].flags = flags;
    g_ofds[ofd_index].refs = 1;

    fd_entry_t* entry = &processes[pid].fds[fd];
    entry->kind = FD_ENTRY_FILE;
    entry->ofd = ofd_index;
    entry->flags = 0;
    return fd;
}

int fd_dup(int pid, int oldfd) {
    fd_entry_t* old = fd_lookup(pid, oldfd);
    if (!old || old->kind == FD_ENTRY_FREE) return -1;

    int fd = -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (processes[pid].fds[i].kind == FD_ENTRY_FREE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    open_file_t* ofd = fd_entry_ofd(old);
    if (old->kind == FD_ENTRY_FILE) {
        if (!ofd || fs_slot_add_ref(ofd->file_slot) < 0) return -1;
        ofd->refs++;
    }

    processes[pid].fds[fd] = *old;
    return fd;
}

void fd_close_all(process_t* p) {
    if (!p) return;

    for (int fd = 0; fd < FD_MAX; fd++) {
        if (p->fds[fd].kind != FD_ENTRY_FREE) fd_release_entry(&p->fds[fd]);
    }
}

int fd_clone_process(const process_t* parent, process_t* child) {
    if (!parent || !child) return -1;

    for (int fd = 0; fd < FD_MAX; fd++) {
        child->fds[fd].kind = FD_ENTRY_FREE;
        child->fds[fd].ofd = -1;
        child->fds[fd].flags = 0;
    }

    for (int fd = 0; fd < FD_MAX; fd++) {
        const fd_entry_t* src = &parent->fds[fd];
        if (src->kind == FD_ENTRY_FILE) {
            open_file_t* ofd = fd_entry_ofd(src);
            if (!ofd || fs_slot_add_ref(ofd->file_slot) < 0) {
                fd_close_all(child);
                return -1;
            }
            ofd->refs++;
        }
        child->fds[fd] = *src;
    }

    return 0;
}

int fd_close(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind == FD_ENTRY_FREE) return -1;

    fd_release_entry(entry);
    return 0;
}

int fd_read(int pid, int fd, void* buf, size_t count) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || (!buf && count > 0)) return -1;

    if (entry->kind == FD_ENTRY_CONSOLE_IN) {
        size_t n = 0;
        while (n < count) {
            int c = kbd_getchar();
            if (c < 0) break;
            ((char*)buf)[n++] = (char)c;
            if (c == '\n') break;
        }
        return (int)n;
    }

    if (entry->kind != FD_ENTRY_FILE) return -1;

    open_file_t* ofd = fd_entry_ofd(entry);
    if (!ofd) return -1;
    if ((ofd->flags & O_ACCMODE) == O_WRONLY) return -1;

    int n = fs_slot_read(ofd->file_slot, ofd->offset, buf, count);
    if (n < 0) return -1;
    ofd->offset += (uint32_t)n;
    return n;
}

int fd_write(int pid, int fd, const void* buf, size_t count) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || (!buf && count > 0)) return -1;

    if (entry->kind == FD_ENTRY_CONSOLE_OUT ||
        entry->kind == FD_ENTRY_CONSOLE_ERR) {
        terminal_write((const char*)buf, count);
        return (int)count;
    }

    if (entry->kind != FD_ENTRY_FILE) return -1;

    open_file_t* ofd = fd_entry_ofd(entry);
    if (!ofd) return -1;
    if ((ofd->flags & O_ACCMODE) == O_RDONLY) return -1;

    uint32_t offset = ofd->offset;
    if (ofd->flags & O_APPEND) {
        int size = fs_slot_size(ofd->file_slot);
        if (size < 0) return -1;
        offset = (uint32_t)size;
    }

    int n = fs_slot_write(ofd->file_slot, offset, buf, count);
    if (n < 0) return -1;
    ofd->offset = offset + (uint32_t)n;
    return n;
}

int fd_seek(int pid, int fd, int64_t offset, int whence) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    open_file_t* ofd = fd_entry_ofd(entry);
    if (!ofd) return -1;

    int size = fs_slot_size(ofd->file_slot);
    if (size < 0) return -1;

    int64_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (int64_t)ofd->offset;
        break;
    case SEEK_END:
        base = (int64_t)size;
        break;
    default:
        return -1;
    }

    if (offset > 0 && base > INT64_MAX - offset) return -1;
    if (offset < 0 && base < INT64_MIN - offset) return -1;
    int64_t target = base + offset;

    if (target < 0 || target > (int64_t)size) return -1;

    ofd->offset = (uint32_t)target;
    return (int)target;
}

int fd_size(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    open_file_t* ofd = fd_entry_ofd(entry);
    if (!ofd) return -1;
    return fs_slot_size(ofd->file_slot);
}

int fd_truncate(int pid, int fd) {
    fd_entry_t* entry = fd_lookup(pid, fd);
    if (!entry || entry->kind != FD_ENTRY_FILE) return -1;

    open_file_t* ofd = fd_entry_ofd(entry);
    if (!ofd) return -1;
    if (fs_slot_truncate(ofd->file_slot) < 0) return -1;

    ofd->offset = 0;
    return 0;
}
