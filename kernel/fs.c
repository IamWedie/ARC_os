/*
 * ARC OS - filesystem.
 *
 * A file-data arena of 512-byte blocks managed with a free-block bitmap.
 * Each file owns a contiguous block extent plus metadata.
 *
 * When an ATA disk is present the filesystem is made persistent: the
 * superblock lives at sector 0, the file table at sectors 1..8 and file
 * data at FS_DISK_DATA_LBA + block. Mutations are written through to the
 * disk so files survive a reboot; without a disk it degrades to the old
 * RAM-only behaviour.
 * File descriptors are the descriptor-table indices + 3 so that 0/1/2
 * stay stdin/stdout/stderr for the console.
 */
#include "kernel.h"

#define FS_BLOCK_SIZE 512
#define FS_ARENA_BYTES (512 * 1024)
#define FS_NUM_BLOCKS (FS_ARENA_BYTES / FS_BLOCK_SIZE)   /* 1024 */
#define FS_BITMAP_BYTES ((FS_NUM_BLOCKS + 7) / 8)

/* On-disk layout (sector granularity). */
#define FS_DISK_SB_LBA   0                        /* superblock             */
#define FS_DISK_DIR_LBA  1                        /* fs_files[] table (raw) */
#define FS_DIR_SECTORS   ((sizeof(fs_file_t) * FS_MAX_FILES + 511) / 512)
#define FS_DISK_DATA_LBA (FS_DISK_DIR_LBA + FS_DIR_SECTORS)
#define FS_DISK_VERSION  1

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t block_size;
    uint32_t num_blocks;
    uint32_t data_start_lba;
} fs_sb_t;

static fs_file_t* fs_files;
static uint8_t* fs_bitmap;
static uint8_t* fs_arena;
static void* fs_dir_scratch;    /* >= FS_DIR_SECTORS*512 for on-disk I/O */
static int fs_disk = 0;     /* nonzero when the FS is backed by the ATA disk */

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

static int fs_disk_fs_local_ok = 0;

/* --- persistence plumbing (write-through to the ATA disk) --- */

static int fs_persist_dir(void) {
    if (!fs_disk || !fs_disk_fs_local_ok) return 0;
    memcpy(fs_dir_scratch, fs_files, sizeof(fs_file_t) * FS_MAX_FILES);
    return ata_write_sectors(FS_DISK_DIR_LBA, FS_DIR_SECTORS, fs_dir_scratch);
}

static int fs_persist_file(fs_file_t* f) {
    if (!fs_disk || !fs_disk_fs_local_ok) return 0;
    if (f->blocks == 0 || f->size == 0) return 0;
    /* File extents are <= FS_MAX_FILESIZE/512 = 128 blocks: fits uint8_t. */
    return ata_write_sectors(FS_DISK_DATA_LBA + f->start_block,
                             (uint8_t)f->blocks,
                             fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE);
}

static void fs_format(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_files[i].used = 0;
        fs_files[i].name[0] = '\0';
        fs_files[i].size = 0;
        fs_files[i].start_block = 0;
        fs_files[i].blocks = 0;
        fs_files[i].pos = 0;
    }
    if (fs_disk) {
        /* Sector I/O always operates on full 512-byte units: never hand the
         * (packed, 24-byte) superblock struct to ata_* directly or the
         * transfer overruns its stack local. Stage it in a real sector. */
        uint8_t sb[FS_SECTOR_SIZE];
        memset(sb, 0, sizeof(sb));
        fs_sb_t* sp = (fs_sb_t*)(void*)sb;
        sp->magic[0] = 'A'; sp->magic[1] = 'R'; sp->magic[2] = 'C'; sp->magic[3] = 'F';
        sp->magic[4] = 'S'; sp->magic[5] = '\0'; sp->magic[6] = '\0'; sp->magic[7] = '\0';
        sp->version = FS_DISK_VERSION;
        sp->block_size = FS_BLOCK_SIZE;
        sp->num_blocks = FS_NUM_BLOCKS;
        sp->data_start_lba = FS_DISK_DATA_LBA;
        ata_write_sectors(FS_DISK_SB_LBA, 1, sb);
        fs_persist_dir();
    }
}

static __attribute__((always_inline)) inline void fs_check_ret(uint64_t saved) {
    uint64_t now;
    __asm__ volatile("movq 8(%%rbp), %0" : "=r"(now));
    if (now != saved) {
        terminal_putstring("*** fs_init ret CORRUPTED: was 0x");
        terminal_print_hex(saved);
        terminal_putstring(" now 0x");
        terminal_print_hex(now);
        terminal_putstring("\n");
        for(;;) __asm__ volatile("cli; hlt");
    }
}

void fs_init(void) {
    uint64_t fs_ret_saved;
    __asm__ volatile("movq 8(%%rbp), %0" : "=r"(fs_ret_saved));
    fs_files = kmalloc(sizeof(fs_file_t) * FS_MAX_FILES);
    fs_bitmap = kmalloc(FS_BITMAP_BYTES);
    memset(fs_bitmap, 0, FS_BITMAP_BYTES);
    fs_arena = pmm_alloc_pages(FS_ARENA_BYTES / 4096);
    fs_dir_scratch = pmm_alloc_pages(1);
    if (!fs_files || !fs_bitmap || !fs_arena || !fs_dir_scratch) return;

    fs_disk = ata_present();

    int mounted = 0;
    if (fs_disk) {
        uint8_t sb[FS_SECTOR_SIZE];
        fs_sb_t* sp = (fs_sb_t*)(void*)sb;
        int ok = (ata_read_sectors(FS_DISK_SB_LBA, 1, sb) == 0 &&
                  sp->magic[0] == 'A' && sp->magic[1] == 'R' &&
                  sp->magic[2] == 'C' && sp->magic[3] == 'F' &&
                  sp->magic[4] == 'S' &&
                  sp->version == FS_DISK_VERSION &&
                  sp->block_size == FS_BLOCK_SIZE &&
                  sp->num_blocks == FS_NUM_BLOCKS &&
                  sp->data_start_lba == FS_DISK_DATA_LBA);
        if (ok) {
            ok = ata_read_sectors(FS_DISK_DIR_LBA, FS_DIR_SECTORS, fs_dir_scratch) == 0;
            if (ok)
                memcpy(fs_files, fs_dir_scratch, sizeof(fs_file_t) * FS_MAX_FILES);
            int chunk = 128;
            for (uint32_t off = 0; ok && off < FS_NUM_BLOCKS; off += chunk) {
                int n = FS_NUM_BLOCKS - (int)off;
                if (n > chunk) n = chunk;
                if (ata_read_sectors(FS_DISK_DATA_LBA + off, (uint8_t)n,
                                     fs_arena + off * FS_BLOCK_SIZE) != 0)
                    ok = 0;
            }
        }
        if (ok) {
            /* Rebuild the block bitmap from the loaded file table. */
            int used = 0;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (!fs_files[i].used) continue;
                used++;
                fs_files[i].pos = 0;
                for (uint32_t j = 0; j < fs_files[i].blocks; j++)
                    fs_bmap_set((int)fs_files[i].start_block + (int)j, 1);
            }
            mounted = 1;
            terminal_putstring("FS: mounted from disk, ");
            terminal_print_int(used);
            terminal_putstring(" files\n");
        }
    }

    if (!mounted) {
        fs_format();
        if (fs_disk)
            terminal_putstring("FS: formatted new volume\n");
        else
            terminal_putstring("FS: RAM-only (no ATA disk)\n");

        /* Seed a welcome file so the filesystem is demonstrably working. */
        const char* text =
            "Welcome to ARC OS!\n"
            "Commands: help, clear, ls, echo, cat <file>, rm <file>\n";
        uint32_t len = 0;
        while (text[len]) len++;
        int fd = fs_open("welcome.txt", 1);
        fs_write(fd, text, len);
    }

    fs_disk_fs_local_ok = 1;
    fs_check_ret(fs_ret_saved);
    terminal_putstring("FS: ret-ok pre-selftest\n");
    fs_selftest();
    fs_check_ret(fs_ret_saved);
    terminal_putstring("FS: ret-ok post-selftest\n");
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
            fs_persist_dir();
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
    fs_persist_file(f);
    fs_persist_dir();
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
    fs_persist_dir();
    return 0;
}

int fs_file_size(int fd) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    return (int)fs_files[slot].size;
}

int fs_truncate(int fd) {
    int slot = fd - FS_SLOT_FIRST;
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    fs_file_t* f = &fs_files[slot];
    if (f->blocks > 0) fs_free_extent((int)f->start_block, (int)f->blocks);
    f->size = 0;
    f->blocks = 0;
    f->start_block = 0;
    f->pos = 0;
    fs_persist_dir();
    return 0;
}

int fs_rename(const char* oldname, const char* newname) {
    int old = fs_find_slot(oldname);
    if (old < 0) return -1;
    int same = fs_find_slot(newname);
    if (same >= 0 && same != old) {
        if (fs_delete(newname) != 0) return -1;
    }
    fs_name_copy(&fs_files[old], newname);
    fs_persist_dir();
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