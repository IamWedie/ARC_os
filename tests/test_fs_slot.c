/*
 * ARC OS real filesystem slot tests
 *
 * Links against the REAL kernel/fs.c with tests/fs_stub.c providing the heap,
 * ATA disk, and console, and tests/pmm_stub.c providing physical frames. These
 * tests pin the backend contract the descriptor layer depends on:
 * fs_open_slot() takes no reference, each fs_slot_add_ref() takes one, a
 * deleted-but-open slot keeps its data until the last release, deleted names
 * are hidden and can be reused by a new file, and mount normalization clears
 * stale deleted entries and their extents.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef SYS_OPEN /* MinGW stdio uses this name for _SYS_OPEN */
#include "kernel.h"

/* Internal helper exported by kernel/fs.c for this host suite. */
int fs_find_slot(const char* name);

extern void fs_stub_disk_reset(int present);
extern void fs_stub_disk_write_bytes(uint32_t lba, const void* data, size_t len);
extern const char* fs_stub_console_text(void);

#define TEST_BLOCK_SIZE 512
#define TEST_NUM_BLOCKS 1024
#define TEST_DIR_SECTORS ((sizeof(fs_file_t) * FS_MAX_FILES + 511) / 512)
#define TEST_DATA_LBA (1 + TEST_DIR_SECTORS)
#define TEST_MOUNT_VERSION 2

/* Mirrors kernel/fs.c's private fs_sb_t. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t block_size;
    uint32_t num_blocks;
    uint32_t data_start_lba;
} test_sb_t;

static int tests_passed;
static int tests_failed;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (cond) {                                           \
            tests_passed++;                                   \
        } else {                                              \
            tests_failed++;                                   \
            printf("  FAIL: ");                               \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static void mount_ram(void) {
    fs_stub_disk_reset(0);
    fs_init();
}

/*
 * Prove the allocator has no leaked blocks by filling the whole 512 KiB arena
 * with eight 64 KiB files. A leaked single-block extent makes the last
 * allocation fail.
 */
static void check_all_blocks_free(void) {
    static char filler[FS_MAX_FILESIZE];
    memset(filler, 'Z', sizeof(filler));

    int wrote = 0;
    for (int i = 0; i < TEST_NUM_BLOCKS / (FS_MAX_FILESIZE / TEST_BLOCK_SIZE); i++) {
        char name[16];
        snprintf(name, sizeof(name), "fill%d", i);
        int slot = fs_open_slot(name, 1);
        if (slot < 0) break;
        if (fs_slot_write(slot, 0, filler, sizeof(filler)) != (int)sizeof(filler))
            break;
        wrote++;
    }

    CHECK(wrote == TEST_NUM_BLOCKS / (FS_MAX_FILESIZE / TEST_BLOCK_SIZE),
          "arena not fully free: filled %d of %d 64 KiB files", wrote,
          TEST_NUM_BLOCKS / (FS_MAX_FILESIZE / TEST_BLOCK_SIZE));
}

static void test_slot_refcount_and_deferred_unlink(void) {
    printf("\n=== Test 1: slot refcount and deferred unlink ===\n");
    mount_ram();

    /* Remove the seeded file so the whole arena starts free. */
    CHECK(fs_delete("welcome.txt") == 0, "delete welcome.txt failed");

    const char* payload = "deferred unlink payload";
    uint32_t len = (uint32_t)strlen(payload);

    int slot = fs_open_slot("victim", 1);
    CHECK(slot >= 0, "open_slot(victim, create) = %d", slot);
    if (slot < 0) return;

    int wrote = fs_slot_write(slot, 0, payload, len);
    CHECK(wrote == (int)len, "write to victim returned %d", wrote);
    CHECK(fs_slot_add_ref(slot) == 0, "first add_ref failed");
    CHECK(fs_slot_add_ref(slot) == 0, "second add_ref failed");

    CHECK(fs_delete("victim") == 0, "delete of open victim failed");
    CHECK(fs_find_slot("victim") == -1,
          "deleted-but-open victim still resolves by name");

    char buf[64] = {0};
    int got = fs_slot_read(slot, 0, buf, len);
    CHECK(got == (int)len, "read after delete returned %d", got);
    CHECK(memcmp(buf, payload, len) == 0,
          "data changed after delete: \"%.*s\"", (int)len, buf);

    fs_slot_release(slot);
    memset(buf, 0, sizeof(buf));
    got = fs_slot_read(slot, 0, buf, len);
    CHECK(got == (int)len, "read after one release returned %d", got);
    CHECK(memcmp(buf, payload, len) == 0,
          "data changed after one release: \"%.*s\"", (int)len, buf);
    CHECK(fs_slot_size(slot) == (int)len,
          "size after one release = %d", fs_slot_size(slot));

    fs_slot_release(slot);
    CHECK(fs_slot_size(slot) == -1,
          "last release did not free the slot (size = %d)",
          fs_slot_size(slot));
    CHECK(fs_slot_read(slot, 0, buf, 1) == -1,
          "read after last release was accepted");

    check_all_blocks_free();
}

static void test_hidden_deleted_name_reuse(void) {
    printf("\n=== Test 2: deleted name hidden and reusable ===\n");
    mount_ram();

    int oldslot = fs_open_slot("victim", 1);
    CHECK(oldslot >= 0, "open_slot(victim, create) = %d", oldslot);
    if (oldslot < 0) return;

    CHECK(fs_slot_write(oldslot, 0, "OLD", 3) == 3, "old write failed");
    CHECK(fs_slot_add_ref(oldslot) == 0, "add_ref failed");
    CHECK(fs_delete("victim") == 0, "delete while open failed");

    CHECK(fs_find_slot("victim") == -1,
          "fs_find_slot still resolves the deleted name");
    CHECK(fs_open_slot("victim", 0) == -1,
          "open without create resolved the deleted name");
    CHECK(fs_rename("victim", "other") == -1,
          "rename accepted the deleted name");

    int newslot = fs_open_slot("victim", 1);
    CHECK(newslot >= 0, "re-create of victim failed: %d", newslot);
    CHECK(newslot != oldslot, "re-create reused the deleted slot %d", oldslot);
    CHECK(fs_slot_size(newslot) == 0,
          "re-created victim size = %d", fs_slot_size(newslot));
    CHECK(fs_find_slot("victim") == newslot,
          "fs_find_slot(victim) = %d, want new slot %d",
          fs_find_slot("victim"), newslot);

    CHECK(fs_slot_write(newslot, 0, "NEW", 3) == 3, "new write failed");

    char oldbuf[8] = {0};
    char newbuf[8] = {0};
    int got = fs_slot_read(oldslot, 0, oldbuf, 3);
    CHECK(got == 3, "old reader read returned %d", got);
    CHECK(memcmp(oldbuf, "OLD", 3) == 0,
          "old reader sees \"%.3s\" instead of OLD", oldbuf);
    got = fs_slot_read(newslot, 0, newbuf, 3);
    CHECK(got == 3, "new reader read returned %d", got);
    CHECK(memcmp(newbuf, "NEW", 3) == 0,
          "new reader sees \"%.3s\" instead of NEW", newbuf);

    fs_slot_release(oldslot);
    CHECK(fs_slot_size(oldslot) == -1,
          "releasing the only old reference did not free the slot");
}

static void build_deleted_image(void) {
    fs_stub_disk_reset(1);

    uint8_t sb[FS_SECTOR_SIZE];
    memset(sb, 0, sizeof(sb));
    test_sb_t* sp = (test_sb_t*)(void*)sb;
    sp->magic[0] = 'A';
    sp->magic[1] = 'R';
    sp->magic[2] = 'C';
    sp->magic[3] = 'F';
    sp->magic[4] = 'S';
    sp->version = TEST_MOUNT_VERSION;
    sp->block_size = TEST_BLOCK_SIZE;
    sp->num_blocks = TEST_NUM_BLOCKS;
    sp->data_start_lba = TEST_DATA_LBA;
    fs_stub_disk_write_bytes(0, sb, sizeof(sb));

    uint8_t dir[TEST_DIR_SECTORS * FS_SECTOR_SIZE];
    memset(dir, 0, sizeof(dir));
    fs_file_t* files = (fs_file_t*)(void*)dir;
    files[0].used = 1;
    files[0].deleted = 1;
    files[0].open_refs = 3;   /* stale pre-reboot references */
    files[0].pos = 7;
    files[0].size = 1;
    files[0].start_block = 0;
    files[0].blocks = 1;
    strcpy(files[0].name, "ghost");
    fs_stub_disk_write_bytes(1, dir, sizeof(dir));

    uint8_t data = 'X';
    fs_stub_disk_write_bytes(TEST_DATA_LBA, &data, 1);
}

static void test_mount_normalizes_deleted_open(void) {
    printf("\n=== Test 3: mount normalizes deleted-but-open entries ===\n");
    build_deleted_image();
    fs_init();

    CHECK(strstr(fs_stub_console_text(), "FS: mounted from disk") != NULL,
          "fs_init did not mount the fake version-2 image");
    CHECK(fs_find_slot("ghost") == -1,
          "stale deleted name still resolves after mount");
    CHECK(fs_slot_size(0) == -1,
          "stale deleted slot 0 is still live (size = %d)", fs_slot_size(0));
    CHECK(fs_slot_add_ref(0) == -1,
          "stale deleted slot 0 accepted a reference after mount");

    check_all_blocks_free();
}

int main(void) {
    printf("ARC OS real filesystem slot tests\n");
    test_slot_refcount_and_deferred_unlink();
    test_hidden_deleted_name_reuse();
    test_mount_normalizes_deleted_open();

    printf("\n=== fs slot suite: %d checks, %d failures ===\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
