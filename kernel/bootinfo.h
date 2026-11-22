#ifndef ARC_BOOTINFO_H
#define ARC_BOOTINFO_H

#include <stdint.h>

#define BOOT_MAGIC_MB1 0x2BADB002u
#define BOOT_MAGIC_MB2 0x36D76289u

#define BOOTINFO_MAX_MMAP 64

#define BOOTINFO_MMAP_AVAILABLE        1u
#define BOOTINFO_MMAP_RESERVED         2u
#define BOOTINFO_MMAP_ACPI_RECLAIMABLE 3u
#define BOOTINFO_MMAP_NVS              4u
#define BOOTINFO_MMAP_BADRAM           5u

struct bootinfo_mmap_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t pad;
};

struct bootinfo_framebuffer {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type; /* 1=indexed, 2=RGB, 3=EGA text */
    uint8_t red_pos;
    uint8_t red_mask;
    uint8_t green_pos;
    uint8_t green_mask;
    uint8_t blue_pos;
    uint8_t blue_mask;
    uint8_t reserved;
};

struct bootinfo {
    uint32_t magic;
    uint32_t flags;
    struct bootinfo_framebuffer fb;
    uint32_t mmap_count;
    struct bootinfo_mmap_entry mmap[BOOTINFO_MAX_MMAP];
    uint64_t mem_lower_kb;
    uint64_t mem_upper_kb;
};

#define BOOTINFO_FLAG_FB   (1u << 0)
#define BOOTINFO_FLAG_MMAP (1u << 1)
#define BOOTINFO_FLAG_MEM  (1u << 2)

void bootinfo_parse(struct bootinfo* out, uint64_t magic, uint64_t info_addr);

/* Pure helpers exposed for host tests. */
int bootinfo_mb2_parse(struct bootinfo* out, const void* mb2_info, uint32_t max_bytes);
int bootinfo_mb1_parse(struct bootinfo* out, const void* mb1_info);
int bootinfo_normalize_mmap(struct bootinfo* out);

#endif