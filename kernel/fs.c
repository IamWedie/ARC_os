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

/*
 * The return-address integrity check below relies on the kernel's frame
 * pointer and privileged instructions. Hosted builds link this file into the
 * native test suite, where that check is meaningless.
 */
#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
#define FS_HOSTED_BUILD 1
#else
#define FS_HOSTED_BUILD 0
#endif

#define FS_BLOCK_SIZE 512
#define FS_ARENA_BYTES (512 * 1024)
#define FS_NUM_BLOCKS (FS_ARENA_BYTES / FS_BLOCK_SIZE)   /* 1024 */
#define FS_BITMAP_BYTES ((FS_NUM_BLOCKS + 7) / 8)

/* On-disk layout (sector granularity). */
#define FS_DISK_SB_LBA   0                        /* superblock             */
#define FS_DISK_DIR_LBA  1                        /* fs_files[] table (raw) */
#define FS_DIR_SECTORS   ((sizeof(fs_file_t) * FS_MAX_FILES + 511) / 512)
#define FS_DISK_DATA_LBA (FS_DISK_DIR_LBA + FS_DIR_SECTORS)
#define FS_DISK_VERSION  2

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

/*
 * Resolve a visible file name. Deleted-but-open slots keep their name for
 * diagnostics but must never resolve again; host tests call this directly.
 */
int fs_find_slot(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used || fs_files[i].deleted) continue;
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

static void fs_slot_clear(int slot) {
    /* Whole-record zeroing also clears struct padding that is persisted by
     * fs_persist_dir(); field-by-field clearing leaked stale bytes to disk. */
    memset(&fs_files[slot], 0, sizeof(fs_files[slot]));
}

/* Release the storage of an unlinked slot and mark it reusable. */
static void fs_reclaim_deleted(int slot) {
    fs_file_t* f = &fs_files[slot];
    if (f->blocks > 0) fs_free_extent((int)f->start_block, (int)f->blocks);
    fs_slot_clear(slot);
}

static int fs_disk_fs_local_ok = 0;

/* --- persistence plumbing (write-through to the ATA disk) --- */

static int fs_persist_dir(void) {
    if (!fs_disk || !fs_disk_fs_local_ok) return 0;
    /* The directory occupies whole sectors but the record array may be
     * shorter: zero the scratch first so trailing bytes are deterministic. */
    memset(fs_dir_scratch, 0, FS_DIR_SECTORS * FS_SECTOR_SIZE);
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
    /* Zero the full table, including padding, before persisting it. */
    memset(fs_files, 0, sizeof(fs_file_t) * FS_MAX_FILES);
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

#if !FS_HOSTED_BUILD
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
#endif

void fs_init(void) {
#if !FS_HOSTED_BUILD
    uint64_t fs_ret_saved;
    __asm__ volatile("movq 8(%%rbp), %0" : "=r"(fs_ret_saved));
#endif
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
            /* Rebuild the block bitmap from the loaded file table. open_refs
             * and deleted are transient: a reboot cannot preserve open file
             * descriptions, so stale deleted entries are reclaimed now. */
            int used = 0;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                fs_files[i].pos = 0;
                fs_files[i].open_refs = 0;
                if (!fs_files[i].used) {
                    fs_files[i].deleted = 0;
                    continue;
                }
                if (fs_files[i].deleted) {
                    fs_reclaim_deleted(i);
                    continue;
                }
                used++;
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
        int slot = fs_open_slot("welcome.txt", 1);
        if (slot >= 0 && fs_slot_add_ref(slot) == 0) {
            fs_slot_write(slot, 0, text, len);
            fs_slot_release(slot);
        }
    }

    fs_disk_fs_local_ok = 1;
#if !FS_HOSTED_BUILD
    fs_check_ret(fs_ret_saved);
#endif
    terminal_putstring("FS: ret-ok pre-selftest\n");
    fs_selftest();
#if !FS_HOSTED_BUILD
    fs_check_ret(fs_ret_saved);
#endif
    terminal_putstring("FS: ret-ok post-selftest\n");
}

static void fs_selftest(void) {
    const char* payload = "ARC FS selftest payload: 0123456789";
    uint32_t plen = 0;
    while (payload[plen]) plen++;

    int slot = fs_open_slot("selftest.tmp", 1);
    if (slot < 0 || fs_slot_add_ref(slot) < 0) {
        terminal_putstring("FS selftest: open(create) FAIL\n");
        return;
    }
    if (fs_slot_write(slot, 0, payload, plen) != (int)plen) {
        terminal_putstring("FS selftest: write FAIL\n");
        fs_slot_release(slot);
        return;
    }
    fs_slot_release(slot);

    slot = fs_open_slot("selftest.tmp", 0);
    if (slot < 0 || fs_slot_add_ref(slot) < 0) {
        terminal_putstring("FS selftest: open(read) FAIL\n");
        return;
    }
    char readbuf[128];
    int n = fs_slot_read(slot, 0, readbuf, sizeof(readbuf));
    if (n != (int)plen) {
        terminal_putstring("FS selftest: read len FAIL\n");
        fs_slot_release(slot);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (readbuf[i] != payload[i]) {
            terminal_putstring("FS selftest: data mismatch FAIL\n");
            fs_slot_release(slot);
            return;
        }
    }
    fs_slot_release(slot);
    if (fs_delete("selftest.tmp") != 0) { terminal_putstring("FS selftest: delete FAIL\n"); return; }

    /* Multi-block round trip (>512 B so extent allocation + grow path runs). */
    char big[1600];
    for (int i = 0; i < (int)sizeof(big); i++) big[i] = (char)('a' + (i % 26));
    slot = fs_open_slot("big.tmp", 1);
    if (slot < 0 || fs_slot_add_ref(slot) < 0) {
        terminal_putstring("FS selftest: big open FAIL\n");
        return;
    }
    if (fs_slot_write(slot, 0, big, sizeof(big)) != (int)sizeof(big)) {
        terminal_putstring("FS selftest: big write FAIL\n");
        fs_slot_release(slot);
        return;
    }
    fs_slot_release(slot);

    slot = fs_open_slot("big.tmp", 0);
    if (slot < 0 || fs_slot_add_ref(slot) < 0) {
        terminal_putstring("FS selftest: big open2 FAIL\n");
        return;
    }
    char bigread[1700];
    int br = fs_slot_read(slot, 0, bigread, sizeof(bigread));
    if (br != (int)sizeof(big)) {
        terminal_putstring("FS selftest: big read len FAIL\n");
        fs_slot_release(slot);
        return;
    }
    for (int i = 0; i < (int)sizeof(big); i++) {
        if (bigread[i] != big[i]) {
            terminal_putstring("FS selftest: big data FAIL\n");
            fs_slot_release(slot);
            return;
        }
    }
    fs_slot_release(slot);
    if (fs_delete("big.tmp") != 0) { terminal_putstring("FS selftest: big delete FAIL\n"); return; }

    terminal_putstring("FS selftest: PASS\n");
}

/* --- slot-level API used by kernel/fd.c --- */

/*
 * Resolve or create `name` and return its fs_file_t slot index (not a
 * descriptor). Creating does NOT acquire a reference: the caller owns the new
 * open file description and calls fs_slot_add_ref() exactly once for it.
 */
int fs_open_slot(const char* name, int create) {
    if (!name || name[0] == '\0') return -1;

    int slot = fs_find_slot(name);
    if (slot >= 0) return slot;
    if (!create) return -1;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used) continue;
        fs_slot_clear(i);
        fs_files[i].used = 1;
        fs_name_copy(&fs_files[i], name);
        fs_persist_dir();
        return i;
    }
    return -1;
}

int fs_slot_read(int slot, uint32_t off, void* buf, size_t count) {
    if (slot < 0 || slot >= FS_MAX_FILES) return -1;
    fs_file_t* f = &fs_files[slot];
    if (!f->used) return -1;
    if (!buf && count > 0) return -1;
    if (count == 0 || off >= f->size) return 0;

    size_t avail = f->size - off;
    size_t n = count < avail ? count : avail;
    memcpy(buf, fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE + off, n);
    return (int)n;
}

int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count) {
    if (slot < 0 || slot >= FS_MAX_FILES) return -1;
    fs_file_t* f = &fs_files[slot];
    if (!f->used) return -1;
    if (!buf && count > 0) return -1;
    if (count == 0) return 0;
    if (count > FS_MAX_FILESIZE) return -1;
    if (off > f->size) return -1;

    uint32_t end = off + (uint32_t)count;
    if (end > FS_MAX_FILESIZE) return -1;

    uint32_t need_blocks = (end + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
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

    memcpy(fs_arena + (uintptr_t)f->start_block * FS_BLOCK_SIZE + off, buf, count);
    if (end > f->size) f->size = end;
    fs_persist_file(f);
    fs_persist_dir();
    return (int)count;
}

int fs_slot_size(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    return (int)fs_files[slot].size;
}

int fs_slot_truncate(int slot) {
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

int fs_slot_add_ref(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_files[slot].used) return -1;
    fs_file_t* f = &fs_files[slot];
    /* Deleted-but-open slots are on their way out: no new OFD may claim one. */
    if (f->deleted) return -1;
    if (f->open_refs == UINT16_MAX) return -1;
    f->open_refs++;
    return 0;
}

void fs_slot_release(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES) return;
    fs_file_t* f = &fs_files[slot];
    if (!f->used || f->open_refs == 0) return;

    f->open_refs--;
    if (f->open_refs == 0 && f->deleted) {
        fs_reclaim_deleted(slot);
        fs_persist_dir();
    }
}

int fs_delete(const char* name) {
    int slot = fs_find_slot(name);
    if (slot < 0) return -1;
    fs_file_t* f = &fs_files[slot];
    f->deleted = 1;
    if (f->open_refs == 0) fs_reclaim_deleted(slot);
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
        if (!fs_files[i].used || fs_files[i].deleted) continue;
        terminal_putstring("  ");
        terminal_putstring(fs_files[i].name);
        terminal_putstring("  (");
        terminal_print_int(fs_files[i].size);
        terminal_putstring(" bytes)\n");
    }
    return 0;
}