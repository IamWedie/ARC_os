/* Host FAT32 parser/ops test (M4). Links kernel/fat32.c with fat32_stub.c */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#undef SYS_OPEN
#include "kernel.h"

extern void fat32_stub_reset(void);
extern uint8_t* fat32_stub_disk(void);
extern uint32_t fat32_stub_disk_sectors(void);
extern int fat32_stub_write_count(void);
extern void fat32_stub_reset_write_count(void);

static int pass_count, fail_count;
#define CHECK(cond, msg) do { if (cond) { pass_count++; } else { fail_count++; printf("FAIL: %s\n", msg); } } while(0)

static void make_fat32(uint32_t part_lba, uint32_t vol_sectors) {
    uint8_t* img = fat32_stub_disk();
    /* MBR */
    memset(img, 0, 512);
    img[446+4] = 0x0C;
    *(uint32_t*)(img+446+8) = part_lba;
    *(uint32_t*)(img+446+12) = vol_sectors;
    img[510]=0x55; img[511]=0xAA;

    uint8_t* b = img + part_lba * 512;
    memset(b, 0, vol_sectors * 512);
    b[0]=0xEB; b[1]=0x58; b[2]=0x90;
    memcpy(b+3, "MSDOS5.0", 8);
    *(uint16_t*)(b+11)=512;
    b[13]=1; /* spc */
    *(uint16_t*)(b+14)=32; /* reserved */
    b[16]=2; /* fats */
    *(uint16_t*)(b+17)=0;
    *(uint16_t*)(b+19)=0;
    b[21]=0xF8;
    *(uint16_t*)(b+22)=0;
    *(uint16_t*)(b+24)=63;
    *(uint16_t*)(b+26)=255;
    *(uint32_t*)(b+28)=part_lba;
    *(uint32_t*)(b+32)=vol_sectors;
    uint32_t spf=1;
    while (1) {
        uint32_t data = vol_sectors - 32 - 2*spf;
        uint32_t clusters = data / 1;
        uint32_t need = (clusters + 2) * 4;
        uint32_t need_spf = (need + 511) / 512;
        if (need_spf == spf) break;
        spf = need_spf;
    }
    *(uint32_t*)(b+36)=spf;
    *(uint16_t*)(b+40)=0;
    *(uint16_t*)(b+42)=0;
    *(uint32_t*)(b+44)=2; /* root cluster */
    *(uint16_t*)(b+48)=1; /* fsinfo */
    *(uint16_t*)(b+50)=6;
    b[64]=0x80; b[66]=0x29;
    *(uint32_t*)(b+67)=0x12345678;
    memcpy(b+71, "ARCOS      ", 11);
    memcpy(b+82, "FAT32   ", 8);
    b[510]=0x55; b[511]=0xAA;
    /* FSInfo */
    uint8_t* fs = b + 512;
    *(uint32_t*)(fs+0)=0x41615252;
    *(uint32_t*)(fs+484)=0x61417272;
    *(uint32_t*)(fs+488)=0xFFFFFFFF;
    *(uint32_t*)(fs+492)=0xFFFFFFFF;
    *(uint32_t*)(fs+508)=0xAA550000;
    /* FAT */
    uint8_t* fat1 = b + 32*512;
    *(uint32_t*)(fat1+0)=0x0FFFFFF8;
    *(uint32_t*)(fat1+4)=0x0FFFFFFF;
    *(uint32_t*)(fat1+8)=0x0FFFFFF8;
    memcpy(fat1 + spf*512, fat1, 12);
    /* root dir cluster zeroed by memset above */
}

static void test_mount_and_basic(void) {
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    struct blockdev dev;
    memset(&dev,0,sizeof(dev));
    dev.name="ata0"; dev.sector_count=32768;
    int rc = fat32_mount(&dev, 2048, 32768-2048);
    CHECK(rc == 0, "fat32_mount succeeds");
    CHECK(fat32_is_mounted(), "fat32 is mounted");

    int slot = fat32_open_slot("/hello.txt", 1);
    CHECK(slot >= 0, "create hello.txt");
    const char* msg = "hello-fat32";
    int n = fat32_slot_write(slot, 0, msg, (size_t)strlen(msg));
    CHECK(n == (int)strlen(msg), "write hello.txt");
    fat32_slot_add_ref(slot);
    fat32_slot_release(slot);

    int slot2 = fat32_open_slot("/hello.txt", 0);
    CHECK(slot2 >= 0, "open hello.txt");
    char buf[64]; memset(buf,0,sizeof(buf));
    n = fat32_slot_read(slot2, 0, buf, sizeof(buf));
    CHECK(n == (int)strlen(msg), "read length");
    CHECK(strcmp(buf, msg)==0, "read content");
}

static void test_mkdir_readdir_stat(void) {
    int rc = fat32_mkdir("/adir");
    CHECK(rc == 0, "mkdir /adir");
    struct arc_stat st;
    rc = fat32_stat("/adir", &st);
    CHECK(rc == 0, "stat /adir");
    CHECK(st.is_dir == 1, "adir is dir");

    int slot = fat32_open_slot("/adir/f.txt", 1);
    CHECK(slot >= 0, "create /adir/f.txt");
    fat32_slot_write(slot, 0, "x", 1);

    struct arc_dirent de;
    int found = 0;
    for (uint32_t i = 0; i < 16; i++) {
        if (fat32_readdir("/adir", i, &de) != 0) break;
        if (strcmp(de.name, "f.txt") == 0) found = 1;
    }
    CHECK(found, "readdir finds f.txt");
}

static void test_rename_unlink(void) {
    int slot = fat32_open_slot("/old.txt", 1);
    CHECK(slot >= 0, "create old.txt");
    fat32_slot_write(slot, 0, "abc", 3);
    int rc = fat32_rename("/old.txt", "/new.txt");
    CHECK(rc == 0, "rename");
    int slot2 = fat32_open_slot("/old.txt", 0);
    CHECK(slot2 < 0, "old gone");
    slot2 = fat32_open_slot("/new.txt", 0);
    CHECK(slot2 >= 0, "new exists");
    char buf[8]={0}; fat32_slot_read(slot2,0,buf,3);
    CHECK(memcmp(buf,"abc",3)==0, "renamed content");
    rc = fat32_delete("/new.txt");
    CHECK(rc == 0, "unlink new");
    slot2 = fat32_open_slot("/new.txt", 0);
    CHECK(slot2 < 0, "new deleted");
}

static void test_lfn(void) {
    /* long name create/read/readdir */
    int slot = fat32_open_slot("/longfilename_test.txt", 1);
    CHECK(slot >= 0, "create lfn file");
    const char* msg = "lfn-data";
    int n = fat32_slot_write(slot, 0, msg, strlen(msg));
    CHECK(n == (int)strlen(msg), "write lfn file");
    struct arc_stat st;
    int rc = fat32_stat("/longfilename_test.txt", &st);
    CHECK(rc == 0, "stat lfn file");
    CHECK(st.size == (uint32_t)strlen(msg), "lfn size");
    int slot2 = fat32_open_slot("/longfilename_test.txt", 0);
    CHECK(slot2 >= 0, "open lfn file");
    char buf[32]={0};
    n = fat32_slot_read(slot2, 0, buf, sizeof(buf));
    CHECK(n == (int)strlen(msg) && strcmp(buf,msg)==0, "read lfn file");
    struct arc_dirent de; int found = 0;
    for (uint32_t i=0;i<32;i++) { if (fat32_readdir("/", i, &de)!=0) break; if (strcmp(de.name,"longfilename_test.txt")==0) found=1; }
    CHECK(found, "readdir finds lfn name");
    rc = fat32_mkdir("/long_directory_name");
    CHECK(rc == 0, "mkdir lfn");
    rc = fat32_stat("/long_directory_name", &st);
    CHECK(rc == 0 && st.is_dir==1, "stat lfn dir");
}

static void test_corrupt_rejected(void) {
    struct blockdev dev; memset(&dev,0,sizeof(dev)); dev.name="ata0"; dev.sector_count=32768;

    fat32_stub_reset();
    /* no valid BPB */
    int rc = fat32_mount(&dev, 0, 32768);
    CHECK(rc != 0, "corrupt image rejected");

    /* bad signature */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    fat32_stub_disk()[2048*512 + 510] = 0x00;
    rc = fat32_mount(&dev, 2048, 32768-2048);
    CHECK(rc != 0, "bad signature rejected");

    /* bad bytes_per_sector */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    *(uint16_t*)(fat32_stub_disk() + 2048*512 + 11) = 1024;
    rc = fat32_mount(&dev, 2048, 32768-2048);
    CHECK(rc != 0, "bad bytes_per_sector rejected");

    /* bad FAT count */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    fat32_stub_disk()[2048*512 + 16] = 0;
    rc = fat32_mount(&dev, 2048, 32768-2048);
    CHECK(rc != 0, "bad fat count rejected");

    /* bad sectors_per_cluster */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    fat32_stub_disk()[2048*512 + 13] = 0;
    rc = fat32_mount(&dev, 2048, 32768-2048);
    CHECK(rc != 0, "bad spc rejected");
}

static void test_corrupt_fuzz(void) {
    /* Deterministic pseudo-random bitflips should never crash; mount may fail. */
    struct blockdev dev; memset(&dev,0,sizeof(dev)); dev.name="ata0"; dev.sector_count=32768;
    uint32_t seed = 0xC0FFEEu;
    for (int iter = 0; iter < 32; iter++) {
        fat32_stub_reset();
        make_fat32(2048, 32768-2048);
        uint8_t* img = fat32_stub_disk();
        /* flip a few bytes in BPB/FAT/root region */
        for (int f = 0; f < 8; f++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t off = 2048*512 + (seed % (4096u * 512u));
            seed = seed * 1664525u + 1013904223u;
            img[off] ^= (uint8_t)(seed & 0xFF);
        }
        (void)fat32_mount(&dev, 2048, 32768-2048);
        /* If it mounted, exercise basic ops without crashing. */
        if (fat32_is_mounted()) {
            (void)fat32_open_slot("/fuzz.txt", 1);
            (void)fat32_mkdir("/d");
            struct arc_dirent de;
            (void)fat32_readdir("/", 0, &de);
            (void)fat32_delete("/nope.txt");
        }
        CHECK(1, "fuzz iteration survived");
    }
}

static void test_readonly_mount_zero_writes(void) {
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    struct blockdev dev; memset(&dev,0,sizeof(dev));
    dev.name="ahci0"; dev.sector_count=32768; dev.read_only = 0;
    CHECK(fat32_mount(&dev, 2048, 32768-2048) == 0, "ro prep mount");
    int slot = fat32_open_slot("/ro.txt", 1);
    CHECK(slot >= 0, "ro prep create");
    CHECK(fat32_slot_write(slot, 0, "readonly", 8) == 8, "ro prep write");
    fat32_slot_add_ref(slot);
    fat32_slot_release(slot);
    CHECK(fat32_mkdir("/rodir") == 0, "ro prep mkdir");

    /* Remount read-only */
    dev.read_only = 1;
    fat32_stub_reset_write_count();
    CHECK(fat32_mount(&dev, 2048, 32768-2048) == 0, "ro mount");
    CHECK(fat32_stub_write_count() == 0, "ro mount no writes");

    int rs = fat32_open_slot("/ro.txt", 0);
    CHECK(rs >= 0, "ro open existing");
    char buf[16]; memset(buf,0,sizeof(buf));
    CHECK(fat32_slot_read(rs, 0, buf, sizeof(buf)) == 8, "ro read len");
    CHECK(strcmp(buf, "readonly") == 0, "ro read content");
    struct arc_dirent de;
    CHECK(fat32_readdir("/", 0, &de) == 0, "ro readdir");
    CHECK(fat32_stub_write_count() == 0, "ro ops no writes");

    /* writes via VFS/ops must return error, not crash */
    CHECK(fat32_open_slot("/new.txt", 1) < 0, "ro create rejected");
    CHECK(fat32_slot_write(rs, 0, "x", 1) < 0, "ro write rejected");
    CHECK(fat32_slot_truncate(rs) < 0, "ro truncate rejected");
    CHECK(fat32_mkdir("/nope") < 0, "ro mkdir rejected");
    CHECK(fat32_delete("/ro.txt") < 0, "ro unlink rejected");
    CHECK(fat32_rename("/ro.txt", "/x.txt") < 0, "ro rename rejected");
    CHECK(fat32_stub_write_count() == 0, "ro rejected writes no device writes");
}

static void test_arcroot_qualification(void) {
    struct blockdev dev; memset(&dev,0,sizeof(dev));
    dev.name="ahci0"; dev.sector_count=32768;

    /* No label, no marker */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    CHECK(fat32_mount(&dev, 2048, 32768-2048) == 0, "arcroot mount plain");
    CHECK(fat32_qualifies_arcroot() == 0, "arcroot no marker no label");
    fat32_unmount();

    /* Label match */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    uint8_t *img = fat32_stub_disk();
    uint8_t *bpb = img + 2048 * 512;
    memcpy(bpb + 71, "ARCROOT    ", 11);
    CHECK(fat32_mount(&dev, 2048, 32768-2048) == 0, "arcroot mount label");
    CHECK(fat32_qualifies_arcroot() == 1, "arcroot label match");
    fat32_unmount();

    /* Marker file */
    fat32_stub_reset();
    make_fat32(2048, 32768-2048);
    CHECK(fat32_mount(&dev, 2048, 32768-2048) == 0, "arcroot mount marker prep");
    int slot = fat32_open_slot("/ARCROOT.MRK", 1);
    CHECK(slot >= 0, "arcroot create marker");
    fat32_slot_add_ref(slot);
    fat32_slot_release(slot);
    CHECK(fat32_qualifies_arcroot() == 1, "arcroot marker file");
    fat32_unmount();
}

int main(void) {
    test_mount_and_basic();
    test_mkdir_readdir_stat();
    test_rename_unlink();
    test_lfn();
    test_corrupt_rejected();
    test_corrupt_fuzz();
    test_readonly_mount_zero_writes();
    test_arcroot_qualification();
    printf("fat32 tests: %d/%d passed\n", pass_count, pass_count+fail_count);
    return fail_count ? 1 : 0;
}
