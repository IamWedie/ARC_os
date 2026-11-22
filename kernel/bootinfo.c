#include "bootinfo.h"
#include "mboot.h"

#define MB2_TAG_END        0u
#define MB2_TAG_CMDLINE    1u
#define MB2_TAG_BOOTLOADER 2u
#define MB2_TAG_MMAP       6u
#define MB2_TAG_FRAMEBUFFER 8u

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t fb_type;
    uint16_t reserved;
};

struct mb2_fb_rgb {
    uint8_t red_pos;
    uint8_t red_mask_size;
    uint8_t green_pos;
    uint8_t green_mask_size;
    uint8_t blue_pos;
    uint8_t blue_mask_size;
};

static void bi_memset(void* dst, int c, unsigned long n) {
    uint8_t* p = (uint8_t*)dst;
    for (unsigned long i = 0; i < n; i++) p[i] = (uint8_t)c;
}



static void add_mmap(struct bootinfo* out, uint64_t base, uint64_t len, uint32_t type) {
    if (!out || len == 0) return;
    if (out->mmap_count >= BOOTINFO_MAX_MMAP) return;
    struct bootinfo_mmap_entry* e = &out->mmap[out->mmap_count++];
    e->base = base;
    e->len = len;
    e->type = type;
    e->pad = 0;
}

int bootinfo_normalize_mmap(struct bootinfo* out) {
    if (!out) return -1;
    /* Drop zero-length, clamp overflow, and sort by base then type. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < out->mmap_count; i++) {
        struct bootinfo_mmap_entry e = out->mmap[i];
        if (e.len == 0) continue;
        uint64_t end = e.base + e.len;
        if (end < e.base) { /* overflow */
            end = ~0ULL;
            e.len = end - e.base;
        }
        out->mmap[n++] = e;
    }
    out->mmap_count = n;
    /* simple insertion sort (small N) */
    for (uint32_t i = 1; i < out->mmap_count; i++) {
        struct bootinfo_mmap_entry key = out->mmap[i];
        uint32_t j = i;
        while (j > 0) {
            struct bootinfo_mmap_entry* prev = &out->mmap[j - 1];
            if (prev->base < key.base) break;
            if (prev->base == key.base && prev->type <= key.type) break;
            out->mmap[j] = *prev;
            j--;
        }
        out->mmap[j] = key;
    }
    /* Merge adjacent/overlapping available regions; keep non-available as-is
     * but do not merge into available. For simplicity, merge same-type only. */
    if (out->mmap_count == 0) return 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < out->mmap_count; i++) {
        struct bootinfo_mmap_entry e = out->mmap[i];
        if (w == 0) { out->mmap[w++] = e; continue; }
        struct bootinfo_mmap_entry* prev = &out->mmap[w - 1];
        uint64_t prev_end = prev->base + prev->len;
        uint64_t e_end = e.base + e.len;
        if (e.type == prev->type && e.base <= prev_end) {
            if (e_end > prev_end) prev->len = e_end - prev->base;
            continue;
        }
        out->mmap[w++] = e;
    }
    out->mmap_count = w;
    out->flags |= BOOTINFO_FLAG_MMAP;
    return 0;
}

int bootinfo_mb1_parse(struct bootinfo* out, const void* mb1_info) {
    if (!out) return -1;
    bi_memset(out, 0, sizeof(*out));
    out->magic = BOOT_MAGIC_MB1;
    if (!mb1_info) return -1;
    const struct multiboot_info* mi = (const struct multiboot_info*)mb1_info;
    uint32_t flags = mi->flags;
    if (flags & (1u << 0)) {
        out->mem_lower_kb = mi->mem_lower;
        out->mem_upper_kb = mi->mem_upper;
        out->flags |= BOOTINFO_FLAG_MEM;
    }
    if ((flags & (1u << 6)) && mi->mmap_length && mi->mmap_addr) {
        uint32_t left = mi->mmap_length;
        uint64_t p = (uint64_t)mi->mmap_addr;
        while (left >= 20 && out->mmap_count < BOOTINFO_MAX_MMAP) {
            const struct multiboot_mmap_entry* e = (const struct multiboot_mmap_entry*)(uintptr_t)p;
            uint32_t step = e->size + 4u;
            if (step < 20) break;
            if (step > left) break;
            add_mmap(out, e->addr, e->len, e->type);
            p += step;
            left -= step;
        }
        if (out->mmap_count) out->flags |= BOOTINFO_FLAG_MMAP;
    }
    if ((flags & (1u << 12)) && mi->framebuffer_addr && mi->framebuffer_width &&
        mi->framebuffer_height && mi->framebuffer_bpp) {
        out->fb.addr = mi->framebuffer_addr;
        out->fb.pitch = mi->framebuffer_pitch;
        out->fb.width = mi->framebuffer_width;
        out->fb.height = mi->framebuffer_height;
        out->fb.bpp = mi->framebuffer_bpp;
        out->fb.type = mi->framebuffer_type;
        /* Multiboot1 color_info: RGB layout when type==1 (RGB). */
        if (mi->framebuffer_type == 1) {
            out->fb.red_pos = mi->color_info[0];
            out->fb.red_mask = mi->color_info[1];
            out->fb.green_pos = mi->color_info[2];
            out->fb.green_mask = mi->color_info[3];
            out->fb.blue_pos = mi->color_info[4];
            out->fb.blue_mask = mi->color_info[5];
        }
        out->flags |= BOOTINFO_FLAG_FB;
    }
    bootinfo_normalize_mmap(out);
    return 0;
}

int bootinfo_mb2_parse(struct bootinfo* out, const void* mb2_info, uint32_t max_bytes) {
    if (!out) return -1;
    bi_memset(out, 0, sizeof(*out));
    out->magic = BOOT_MAGIC_MB2;
    if (!mb2_info) return -1;
    const uint8_t* base = (const uint8_t*)mb2_info;
    uint32_t total = *(const uint32_t*)base;
    if (total < 16) return -1;
    if (max_bytes && total > max_bytes) total = max_bytes;
    uint32_t off = 8; /* skip total_size + reserved */
    while (off + 8 <= total) {
        const struct mb2_tag* tag = (const struct mb2_tag*)(base + off);
        if (tag->size < 8) break;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MMAP && tag->size >= sizeof(struct mb2_tag_mmap)) {
            const struct mb2_tag_mmap* mm = (const struct mb2_tag_mmap*)tag;
            uint32_t esz = mm->entry_size;
            if (esz >= 24) {
                uint32_t p = off + 16;
                while (p + esz <= off + tag->size && out->mmap_count < BOOTINFO_MAX_MMAP) {
                    const struct mb2_mmap_entry* e = (const struct mb2_mmap_entry*)(base + p);
                    add_mmap(out, e->addr, e->len, e->type);
                    p += esz;
                }
                if (out->mmap_count) out->flags |= BOOTINFO_FLAG_MMAP;
            }
        } else if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= sizeof(struct mb2_tag_framebuffer)) {
            const struct mb2_tag_framebuffer* fb = (const struct mb2_tag_framebuffer*)tag;
            if (fb->addr && fb->width && fb->height && fb->bpp) {
                out->fb.addr = fb->addr;
                out->fb.pitch = fb->pitch;
                out->fb.width = fb->width;
                out->fb.height = fb->height;
                out->fb.bpp = fb->bpp;
                out->fb.type = fb->fb_type;
                if (fb->fb_type == 1 && tag->size >= sizeof(struct mb2_tag_framebuffer) + sizeof(struct mb2_fb_rgb)) {
                    const struct mb2_fb_rgb* rgb = (const struct mb2_fb_rgb*)(fb + 1);
                    out->fb.red_pos = rgb->red_pos;
                    out->fb.red_mask = rgb->red_mask_size;
                    out->fb.green_pos = rgb->green_pos;
                    out->fb.green_mask = rgb->green_mask_size;
                    out->fb.blue_pos = rgb->blue_pos;
                    out->fb.blue_mask = rgb->blue_mask_size;
                }
                out->flags |= BOOTINFO_FLAG_FB;
            }
        }
        uint32_t next = off + ((tag->size + 7u) & ~7u);
        if (next <= off) break;
        off = next;
    }
    bootinfo_normalize_mmap(out);
    return 0;
}

void bootinfo_parse(struct bootinfo* out, uint64_t magic, uint64_t info_addr) {
    if (!out) return;
    bi_memset(out, 0, sizeof(*out));
    if (magic == BOOT_MAGIC_MB2) {
        bootinfo_mb2_parse(out, (const void*)(uintptr_t)info_addr, 0);
        return;
    }
    /* Default to MB1 for legacy compatibility. */
    bootinfo_mb1_parse(out, (const void*)(uintptr_t)info_addr);
}
