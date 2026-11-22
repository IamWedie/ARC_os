#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bootinfo.h"
#include "mboot.h"

static int tests_run, tests_fail;
#define CHECK(cond) do { tests_run++; if (!(cond)) { tests_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void make_mb2_mmap(uint8_t* buf, uint32_t* total) {
    /* Minimal MB2 info: total_size, reserved, mmap tag, end tag */
    uint32_t off = 8;
    /* mmap tag (type 6) */
    struct { uint32_t type,size,entry_size,entry_ver; } hdr = {6, 16 + 24*3, 24, 0};
    memcpy(buf+off, &hdr, 16); off += 16;
    struct { uint64_t base,len; uint32_t type,res; } e[3] = {
        {0x00000000ULL, 0x0009FC00ULL, 1, 0},
        {0x00100000ULL, 0x1FF00000ULL, 1, 0}, /* 511MB */
        {0xFEE00000ULL, 0x00001000ULL, 2, 0}, /* reserved LAPIC */
    };
    memcpy(buf+off, e, sizeof(e)); off += sizeof(e);
    /* end tag */
    uint32_t end[2] = {0, 8}; memcpy(buf+off, end, 8); off += 8;
    *(uint32_t*)buf = off;
    *(uint32_t*)(buf+4) = 0;
    *total = off;
}

static void test_mb2_mmap(void) {
    uint8_t buf[512]; uint32_t total;
    memset(buf, 0, sizeof(buf));
    make_mb2_mmap(buf, &total);
    struct bootinfo bi;
    CHECK(bootinfo_mb2_parse(&bi, buf, sizeof(buf)) == 0);
    CHECK(bi.magic == BOOT_MAGIC_MB2);
    CHECK(bi.flags & BOOTINFO_FLAG_MMAP);
    CHECK(bi.mmap_count == 3);
    CHECK(bi.mmap[0].type == BOOTINFO_MMAP_AVAILABLE);
    CHECK(bi.mmap[0].base == 0x0);
    CHECK(bi.mmap[1].base == 0x100000);
    CHECK(bi.mmap[2].type == BOOTINFO_MMAP_RESERVED);
}

static void test_mb2_framebuffer(void) {
    uint8_t buf[512]; memset(buf, 0, sizeof(buf));
    uint32_t off = 8;
    /* framebuffer tag type 8 */
    struct __attribute__((packed)) {
        uint32_t type, size;
        uint64_t addr; uint32_t pitch, width, height;
        uint8_t bpp, fb_type; uint16_t reserved;
        uint8_t rpos,rmsz,gpos,gmsz,bpos,bmsz;
    } fb = {8, 38, 0xFD000000ULL, 7680, 1920, 1080, 32, 1, 0, 16,8,8,8,0,8};
    memcpy(buf+off, &fb, sizeof(fb)); off += sizeof(fb);
    while (off % 8) off++;
    uint32_t end[2]={0,8}; memcpy(buf+off,end,8); off+=8;
    *(uint32_t*)buf = off; *(uint32_t*)(buf+4)=0;
    struct bootinfo bi;
    CHECK(bootinfo_mb2_parse(&bi, buf, sizeof(buf)) == 0);
    CHECK(bi.flags & BOOTINFO_FLAG_FB);
    CHECK(bi.fb.addr == 0xFD000000ULL);
    CHECK(bi.fb.width == 1920);
    CHECK(bi.fb.height == 1080);
    CHECK(bi.fb.pitch == 7680);
    CHECK(bi.fb.bpp == 32);
    CHECK(bi.fb.type == 1);
    CHECK(bi.fb.red_pos == 16);
    CHECK(bi.fb.blue_pos == 0);
}

static void test_mb1_parse(void) {
    /* Use a static buffer so address is stable; on Win64 it may be >4GB, so
     * build a self-contained image: [multiboot_info][mmap entries] in one
     * heap block we try to keep below 4GB by using a low mmap. */
    static uint8_t backing[4096];
    struct multiboot_info* mi = (struct multiboot_info*)backing;
    memset(backing, 0, sizeof(backing));
    mi->flags = (1u<<0) | (1u<<6);
    mi->mem_lower = 640;
    mi->mem_upper = 523264;
    struct multiboot_mmap_entry* entries = (struct multiboot_mmap_entry*)(backing + sizeof(struct multiboot_info));
    entries[0].size = 20; entries[0].addr = 0x100000; entries[0].len = 0x1FF00000; entries[0].type = 1;
    entries[1].size = 20; entries[1].addr = 0xFEE00000; entries[1].len = 0x1000; entries[1].type = 2;
    struct bootinfo bi;
    if ((uintptr_t)entries <= 0xFFFFFFFFu) {
        mi->mmap_addr = (uint32_t)(uintptr_t)entries;
        mi->mmap_length = 2 * sizeof(struct multiboot_mmap_entry);
        CHECK(bootinfo_mb1_parse(&bi, mi) == 0);
        CHECK(bi.flags & BOOTINFO_FLAG_MMAP);
        CHECK(bi.mmap_count == 2);
        CHECK(bi.mmap[0].type == BOOTINFO_MMAP_AVAILABLE);
    } else {
        mi->flags = (1u<<0);
        CHECK(bootinfo_mb1_parse(&bi, mi) == 0);
    }
    CHECK(bi.magic == BOOT_MAGIC_MB1);
    CHECK(bi.flags & BOOTINFO_FLAG_MEM);
    CHECK(bi.mem_upper_kb == 523264);
}

static void test_normalize_overlaps(void) {
    struct bootinfo bi; memset(&bi,0,sizeof(bi));
    bi.mmap_count = 3;
    bi.mmap[0] = (struct bootinfo_mmap_entry){0x100000, 0x100000, 1, 0};
    bi.mmap[1] = (struct bootinfo_mmap_entry){0x180000, 0x100000, 1, 0}; /* overlap */
    bi.mmap[2] = (struct bootinfo_mmap_entry){0x000000, 0x09FC00, 1, 0}; /* unsorted */
    CHECK(bootinfo_normalize_mmap(&bi) == 0);
    CHECK(bi.mmap_count == 2);
    CHECK(bi.mmap[0].base == 0x0);
    CHECK(bi.mmap[1].base == 0x100000);
    CHECK(bi.mmap[1].len == 0x180000);
}

static void test_normalize_overflow(void) {
    struct bootinfo bi; memset(&bi,0,sizeof(bi));
    bi.mmap_count = 1;
    bi.mmap[0] = (struct bootinfo_mmap_entry){0xFFFFFFFF00000000ULL, 0x200000000ULL, 1, 0};
    CHECK(bootinfo_normalize_mmap(&bi) == 0);
    CHECK(bi.mmap_count == 1);
    CHECK(bi.mmap[0].base == 0xFFFFFFFF00000000ULL);
    CHECK(bi.mmap[0].len == 0xFFFFFFFFULL);
}

int main(void) {
    test_mb2_mmap();
    test_mb2_framebuffer();
    test_mb1_parse();
    test_normalize_overlaps();
    test_normalize_overflow();
    printf("bootinfo tests: %d/%d passed\n", tests_run - tests_fail, tests_run);
    if (tests_fail) { printf("%d FAILED\n", tests_fail); return 1; }
    return 0;
}