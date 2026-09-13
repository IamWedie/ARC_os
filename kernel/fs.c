/*
 * ARC OS - in-memory filesystem (RAM FS).
 *
 * A file-data arena of contiguous frames is managed as 512-byte blocks with a
 * free-block bitmap. Each file owns a contiguous block extent plus metadata.
 * File descriptors are the descriptor-table indices + 3 so that 0/1/2 stay
 * stdin/stdout/stderr for the console.
 */
#include "kernel.h"

#define FS_BLOCK_SIZE 512
#define FS_ARENA_BYTES (512 * 1024)
#define FS_NUM_BLOCKS (FS_ARENA_BYTES / FS_BLOCK_SIZE)   /* 1024 */
#define FS_BITMAP_BYTES ((FS_NUM_BLOCKS + 7) / 8)

static fs_file_t* fs_files;
static uint8_t* fs_bitmap;
static uint8_t* fs_arena;

static void fs_selftest(void);

static int fs_bmap_get(int b)      { return (fs_bitmap[b >> 3] >> (b & 7)) & 1; }
static void fs_bmap_set(int b, int v) {
    if (v) fs_bitmap[b >> 3] |= (1u << (b & 7));
    else   fs_bitmap[b >> 3] &= ~(1u << (b & 7));
}

static int fs_alloc_extent(int n) {
    if (n > FS_NUM_BLOCKS) return -1;
    for (int s = 0; s <= FS_NUM_BLOCKS - n; s++) {
        int ok = 1;
        for (int j = 0; j < n; j++) if (fs_bmap_get(s + j)) { ok = 0; break; }
        if (ok) {
            for (int j = 0; j < n; j++) fs_bmap_set(s + j, 1);
            return s;
        }
    }
    return -1;
}

static void fs_free_extent(int s, int n) {
    for (int j = 0; j < n; j++) fs_bmap_set(s + j, 0);
}

static int fs_find_slot(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used) continue;
        int k = 0;
        while (name[k] && fs_files[i].name[k] &&
               name[k] == fs_files[i].name[k]) k++;
        if (name[k] == '\0' && fs_files[i].name[k] == '\0') return i;
    }
    return -1;
}

static int fs_name_copy(fs_file_t* f, const char* name) {
    int k = 0;
    while (name[k] && k < FS_MAX_FILENAME - 1) {
        f->name[k] = name[k];
        k++;
    }
    f->name[k] = '\0';
    return 0;
}

void fs_init(void) {
    fs_files = kmalloc(sizeof(fs_file_t) * FS_MAX_FILES);
    fs_bitmap = kmalloc(FS_BITMAP_BYTES);
    memset(fs_bitmap, 0, FS_BITMAP_BYTES);
    fs_arena = pmm_alloc_pages(FS_ARENA_BYTES / 4096);
    if (!fs_files || !fs_bitmap || !fs_arena) return;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_files[i].used = 0;
        fs_files[i].name[0] = '\0';
        fs_files[i].size = 0;
        fs_files[i].start_block = 0;
        fs_files[i].blocks = 0;
        fs_files[i].pos = 0;
    }

    /* Seed a welcome file so the filesystem is demonstrably working. */
    const char* text =
        "Welcome to ARC OS!\n"
        "Commands: help, clear, ls, echo, cat <file>, rm <file>\n";
    uint32_t len = 0;
    while (text[len]) len++;
    int fd = fs_open("welcome.txt", 1);
    fs_write(fd, text, len);

    /* Self-test: create/write/read/delete a scratch file. */
    fs_selftest();
}

static void fs_selftest(void) {
    const char* payload = "ARC FS selftest payload: 0123456789";
    uint32_t plen = 0;
    while (payload[plen]) plen++;

    int fd = fs_open("selftest.tmp", 1);
    if (fd < FS_SLOT_FIRST) { terminal_putstring("FS selftest: open(create) FAIL\n"); return; }

    if (fs_write(fd, payload, plen) != (int)plen) {
        terminal_putstring("FS selftest: write FAIL\n");
        return;
    }
    if (fs_close(fd) != 0) { terminal_putstring("FS selftest: close FAIL\n"); return; }

    fd = fs_open("selftest.tmp", 0);
    if (fd < FS_SLOT_FIRST) { terminal_putstring("FS selftest: open(read) FAIL\n"); return; }

    char readbuf[128];
    long n = fs_read(fd, readbuf, sizeof(readbuf));
    if (n != (long)plen) { terminal_putstring("FS selftest: read len FAIL\n"); return; }
    for (long i = 0; i < n; i++) {
        if (readbuf[i] != payload[i]) {
            terminal_putstring("FS selftest: data mismatch FAIL\n");
            return;
        }
    }
    if (fs_close(fd) != 0) { terminal_putstring("FS selftest: close2 FAIL\n"); return; }
    if (fs_delete("selftest.tmp") != 0) { terminal_putstring("FS selftest: delete FAIL\n"); return; }

    /* Multi-block round trip (>512 B so extent allocation + grow path runs). */
    char big[1600];
    for (int i = 0; i < (int)sizeof(big); i++) big[i] = (char)('a' + (i % 26));
    fd = fs_open("big.tmp", 1);
    if (fd < FS_SLOT_FIRST) { terminal_putstring("FS selftest: big open FAIL\n"); return; }
    if (fs_write(fd, big, sizeof(big)) != (int)sizeof(big)) {
        terminal_putstring("FS selftest: big write FAIL\n");
        return;
    }
    if (fs_close(fd) != 0) { terminal_putstring("FS selftest: big close FAIL\n"); return; }
    fd = fs_open("big.tmp", 0);
    char bigread[1700];
    long br = fs_read(fd, bigread, sizeof(bigread));
    if (br != (long)sizeof(big)) { terminal_putstring("FS selftest: big read len FAIL\n"); return; }
    for (int i = 0; i < (int)sizeof(big); i++) {
        if (bigread[i] != big[i]) {
            terminal_putstring("FS selftest: big data FAIL\n");
            return;
        }
    }
    if (fs_delete("big.tmp") != 0) { terminal_putstring("FS selftest: big delete FAIL\n"); return; }

    terminal_putstring("FS selftest: PASS\n");
}

int fs_open(const char* name, int create) {
    int slot = fs_find_slot(name);
    if (slot >= 0) {
        fs_files[slot].pos = 0;
        return FS_SLOT_FIRST + slot;
    }
    if (!create) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used) {
            fs_files[i].used = 1;
            fs_files[i].size = 0;
            fs_files[i].blocks = 0;
            fs_files[i].start_block = 0;
            fs_files[i].pos = 0;
            fs_name_copy(&fs_files[i], name);
            return FS_SLOT_FIRST + i;
        }
    }
    return -1;
}

int fs_read(int fd, void* buf, size_t count) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    fs_file_t* f = &fs_files[slot];
    uint32_t avail = f->size - f->pos;
    size_t n = count;
    if ((uint32_t)n > avail) n = avail;
    memcpy(buf, fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE + f->pos, n);
    f->pos += (uint32_t)n;
    return (int)n;
}

int fs_write(int fd, const void* buf, size_t count) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    if (count > FS_MAX_FILESIZE) return -1;
    fs_file_t* f = &fs_files[slot];

    uint32_t need = f->size + (uint32_t)count;
    uint32_t need_blocks = (need + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;

    if (need_blocks > f->blocks) {
        int nb = fs_alloc_extent((int)need_blocks);
        if (nb < 0) return -1;
        if (f->size > 0)
            memcpy(fs_arena + (uintptr_t)nb * FS_BLOCK_SIZE,
                   fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE, f->size);
        if (f->blocks > 0) fs_free_extent((int)f->start_block, (int)f->blocks);
        f->start_block = (uint32_t)nb;
        f->blocks = need_blocks;
    }

    memcpy(fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE + f->size, buf, count);
    f->size = need;
    return (int)count;
}

int fs_close(int fd) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES) return -1;
    fs_files[slot].pos = 0;
    return 0;
}

int fs_seek(int fd, uint32_t pos) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    if (pos > fs_files[slot].size) return -1;
    fs_files[slot].pos = pos;
    return 0;
}

int fs_delete(const char* name) {
    int slot = fs_find_slot(name);
    if (slot < 0) return -1;
    fs_file_t* f = &fs_files[slot];
    if (f->blocks > 0) fs_free_extent((int)f->start_block, (int)f->blocks);
    f->used = 0;
    f->name[0] = '\0';
    f->size = 0;
    f->blocks = 0;
    f->start_block = 0;
    f->pos = 0;
    return 0;
}

int fs_list(void) {
    terminal_putstring("Files:\n");
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used) continue;
        terminal_putstring("  ");
        terminal_putstring(fs_files[i].name);
        terminal_putstring("  (");
        terminal_print_int(fs_files[i].size);
        terminal_putstring(" bytes)\n");
    }
    return 0;
}