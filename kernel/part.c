/* Minimal MBR/GPT partition probe (M4, read-only). */
#include "kernel.h"

struct gpt_hdr {
    uint8_t sig[8];
    uint32_t rev;
    uint32_t hdr_sz;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t cur_lba;
    uint64_t bak_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t part_lba;
    uint32_t part_count;
    uint32_t part_ent_sz;
    uint32_t part_crc32;
} __attribute__((packed));

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int part_probe(struct blockdev* dev, struct part_info* out, int max) {
    if (!dev || !out || max <= 0) return 0;
    for (int i = 0; i < max; i++) {
        out[i].valid = 0; out[i].lba_start = 0; out[i].sector_count = 0; out[i].type = 0; out[i].gpt_type = 0;
    }

    uint8_t sec[512];
    if (blockdev_read(dev, 0, 1, sec) != 0) return 0;

    int is_gpt = 0;
    uint8_t gsec[512];
    if (blockdev_read(dev, 1, 1, gsec) == 0) {
        if (gsec[0]=='E' && gsec[1]=='F' && gsec[2]=='I' && gsec[3]==' ' &&
            gsec[4]=='P' && gsec[5]=='A' && gsec[6]=='R' && gsec[7]=='T') is_gpt = 1;
    }

    int n = 0;
    if (is_gpt) {
        static const uint8_t GUID_BASIC_DATA[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
        static const uint8_t GUID_ESP[16]        = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
        struct gpt_hdr* h = (struct gpt_hdr*)gsec;
        uint32_t count = h->part_count;
        uint32_t esz = h->part_ent_sz;
        uint64_t plba = h->part_lba;
        if (esz < 128) esz = 128;
        if (count > 128) count = 128;
        uint32_t per_sec = 512 / esz;
        if (per_sec == 0) per_sec = 1;
        for (uint32_t i = 0; i < count && n < max; i++) {
            uint32_t sec_idx = i / per_sec;
            uint32_t off = (i % per_sec) * esz;
            uint8_t ebuf[512];
            if (blockdev_read(dev, (uint32_t)plba + sec_idx, 1, ebuf) != 0) break;
            uint8_t* e = ebuf + off;
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e[k] != 0) { empty = 0; break; }
            if (empty) continue;
            uint64_t first = *(uint64_t*)(e + 32);
            uint64_t last = *(uint64_t*)(e + 40);
            if (last < first) continue;
            out[n].lba_start = (uint32_t)first;
            out[n].sector_count = (uint32_t)(last - first + 1);
            if (guid_eq(e, GUID_ESP)) { out[n].gpt_type = 2; out[n].type = 0xEF; }
            else if (guid_eq(e, GUID_BASIC_DATA)) { out[n].gpt_type = 1; out[n].type = 0xEE; }
            else { out[n].gpt_type = 3; out[n].type = 0xEE; }
            out[n].valid = 1;
            n++;
        }
        return n;
    }

    /* MBR */
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    for (int i = 0; i < 4 && n < max; i++) {
        uint8_t* p = sec + 446 + i * 16;
        uint8_t type = p[4];
        uint32_t lba = *(uint32_t*)(p + 8);
        uint32_t cnt = *(uint32_t*)(p + 12);
        if (type == 0 || cnt == 0) continue;
        out[n].type = type;
        out[n].lba_start = lba;
        out[n].sector_count = cnt;
        out[n].gpt_type = 0;
        out[n].valid = 1;
        n++;
    }
    return n;
}
