/* Minimal VFS dispatch layer (M4). */
#include "kernel.h"

struct vfs_mount_entry {
    char path[VFS_NAME_MAX];
    const struct vfs_ops* ops;
    void* ctx;
    struct blockdev* dev;
    int used;
};

static struct vfs_mount_entry g_mounts[VFS_MAX_MOUNTS];

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int vfs_is_sep(char c) { return c == '/' || c == '\\'; }

static const char* vfs_norm(const char* path, char* out, size_t outsz) {
    size_t j = 0;
    if (!path || !*path) { if (outsz) out[0] = '\0'; return out; }
    /* normalize to leading /, single separators, no trailing slash except root */
    if (!vfs_is_sep(path[0])) { if (j + 1 < outsz) out[j++] = '/'; }
    for (size_t i = 0; path[i] && j + 1 < outsz; i++) {
        char c = path[i];
        if (vfs_is_sep(c)) {
            if (j == 0 || out[j-1] == '/') continue;
            out[j++] = '/';
        } else {
            out[j++] = c;
        }
    }
    if (j > 1 && out[j-1] == '/') j--;
    out[j] = '\0';
    return out;
}

int vfs_mount(const char* path, const struct vfs_ops* ops, void* ctx, struct blockdev* dev) {
    char norm[VFS_NAME_MAX];
    vfs_norm(path, norm, sizeof(norm));
    if (!ops) return -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used && streq(g_mounts[i].path, norm)) {
            g_mounts[i].ops = ops; g_mounts[i].ctx = ctx; g_mounts[i].dev = dev; return 0;
        }
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            size_t k = 0; while (norm[k] && k < VFS_NAME_MAX-1) { g_mounts[i].path[k]=norm[k]; k++; }
            g_mounts[i].path[k]='\0';
            g_mounts[i].ops = ops; g_mounts[i].ctx = ctx; g_mounts[i].dev = dev; g_mounts[i].used = 1;
            return 0;
        }
    }
    return -1;
}

static struct vfs_mount_entry* vfs_find_mount(const char* path, const char** rel) {
    char norm[VFS_NAME_MAX];
    vfs_norm(path, norm, sizeof(norm));
    struct vfs_mount_entry* best = 0;
    size_t best_len = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) continue;
        size_t mlen = 0; while (g_mounts[i].path[mlen]) mlen++;
        /* match exact or prefix / */
        size_t n = 0; while (g_mounts[i].path[n] && norm[n] && g_mounts[i].path[n]==norm[n]) n++;
        if (n != mlen) continue;
        if (norm[mlen] != '\0' && norm[mlen] != '/') continue;
        if (mlen >= best_len) { best = &g_mounts[i]; best_len = mlen; }
    }
    if (rel) {
        static char relbuf[VFS_NAME_MAX];
        if (!best) { relbuf[0]='/'; relbuf[1]='\0'; *rel = relbuf; return 0; }
        const char* r = norm + best_len;
        if (*r == '/') r++;
        size_t j = 0;
        if (*r == '\0') { relbuf[j++] = '/'; }
        else { relbuf[j++] = '/'; while (*r && j + 1 < sizeof(relbuf)) relbuf[j++] = *r++; }
        relbuf[j] = '\0';
        *rel = relbuf;
    }
    return best;
}

int vfs_stat(const char* path, struct arc_stat* st) {
    if (!st) return -1;
    const char* rel; struct vfs_mount_entry* m = vfs_find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->stat) return -1;
    return m->ops->stat(m->ctx, rel, st);
}

int vfs_readdir(const char* path, uint32_t index, struct arc_dirent* out) {
    if (!out) return -1;
    const char* rel; struct vfs_mount_entry* m = vfs_find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    return m->ops->readdir(m->ctx, rel, index, out);
}

int vfs_mkdir(const char* path) {
    const char* rel; struct vfs_mount_entry* m = vfs_find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->mkdir) return -1;
    return m->ops->mkdir(m->ctx, rel);
}

int vfs_unlink(const char* path) {
    const char* rel; struct vfs_mount_entry* m = vfs_find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->unlink) return -1;
    return m->ops->unlink(m->ctx, rel);
}

int vfs_mount_device(const char* devname, const char* fstype, const char* target) {
    struct blockdev* dev = blockdev_find(devname);
    if (!dev) return -1;
    if (streq(fstype, "fat32")) {
        /* mount first partition for simplicity */
        struct part_info parts[4];
        int n = part_probe(dev, parts, 4);
        uint32_t lba = 0, cnt = dev->sector_count;
        for (int i = 0; i < n; i++) {
            if (parts[i].valid && (parts[i].type == 0x0C || parts[i].type == 0x0B)) {
                lba = parts[i].lba_start; cnt = parts[i].sector_count; break;
            }
        }
        if (fat32_mount(dev, lba, cnt) != 0) return -1;
        return 0; /* fat32_mount registers itself */
    }
    (void)target;
    return -1;
}

int vfs_sync(const char* mount) {
    if (!mount) return -1;
    char norm[VFS_NAME_MAX];
    vfs_norm(mount, norm, sizeof(norm));
    struct vfs_mount_entry* m = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used && streq(g_mounts[i].path, norm)) { m = &g_mounts[i]; break; }
    }
    if (!m) return -1;
    if (m->ops && m->ops->sync) {
        if (m->ops->sync(m->ctx) != 0) return -1;
    }
    if (m->dev) {
        if (blockdev_flush(m->dev) != 0) return -1;
    }
    return 0;
}

int vfs_sync_all(void) {
    int rc = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) continue;
        if (vfs_sync(g_mounts[i].path) != 0) rc = -1;
    }
    return rc;
}