/* Minimal FAT32 backend for M4 persistence. */
#include "kernel.h"

#define FAT32_MAX_SLOTS 32
#define FAT32_MAX_NAME 64
#define FAT32_SECTOR 512

struct fat32_bpb {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t part_lba;
};

struct fat32_slot {
    int used;
    char path[FAT32_MAX_NAME];
    uint32_t dir_cluster;
    uint32_t dir_index; /* entry index in directory */
    uint32_t first_cluster;
    uint32_t size;
    uint32_t is_dir;
    uint16_t refs;
    uint8_t deleted;
};

static struct fat32_bpb g_bpb;
static struct blockdev* g_dev;
static int g_mounted;
static struct fat32_slot g_slots[FAT32_MAX_SLOTS];

static int streq(const char* a, const char* b) { while(*a&&*b){if(*a!=*b)return 0;a++;b++;} return *a==*b; }
static void strcopy(char* dst, const char* src, size_t n){ size_t i=0; if(!n) return; while(src[i]&&i+1<n){dst[i]=src[i];i++;} dst[i]='\0'; }
static int is_sep(char c){ return c=='/'||c=='\\'; }

static int rdsec(uint32_t lba, void* buf) { return blockdev_read(g_dev, g_bpb.part_lba + lba, 1, buf); }
static int wrsec(uint32_t lba, const void* buf) { return blockdev_write(g_dev, g_bpb.part_lba + lba, 1, buf); }

static uint32_t clus_lba(uint32_t clus) {
    return g_bpb.data_start_lba + (clus - 2) * g_bpb.sectors_per_cluster;
}

static int rdclus(uint32_t clus, uint8_t* buf) {
    for (uint32_t i = 0; i < g_bpb.sectors_per_cluster; i++)
        if (rdsec(clus_lba(clus)+i, buf + i*FAT32_SECTOR) != 0) return -1;
    return 0;
}

static int wrclus(uint32_t clus, const uint8_t* buf) {
    for (uint32_t i = 0; i < g_bpb.sectors_per_cluster; i++)
        if (wrsec(clus_lba(clus)+i, buf + i*FAT32_SECTOR) != 0) return -1;
    return 0;
}

static uint32_t fat_get(uint32_t clus) {
    uint8_t sec[FAT32_SECTOR];
    uint32_t off = clus * 4;
    uint32_t sec_idx = g_bpb.fat_start_lba + (off / FAT32_SECTOR);
    if (rdsec(sec_idx, sec) != 0) return 0x0FFFFFF7u;
    uint32_t val = *(uint32_t*)(sec + (off % FAT32_SECTOR));
    return val & 0x0FFFFFFFu;
}

static int fat_set(uint32_t clus, uint32_t val) {
    uint32_t off = clus * 4;
    uint32_t sec_idx = off / FAT32_SECTOR;
    uint32_t sec_off = off % FAT32_SECTOR;
    for (uint8_t f = 0; f < g_bpb.fat_count; f++) {
        uint8_t sec[FAT32_SECTOR];
        uint32_t lba = g_bpb.fat_start_lba + f * g_bpb.sectors_per_fat + sec_idx;
        if (rdsec(lba, sec) != 0) return -1;
        uint32_t* p = (uint32_t*)(sec + sec_off);
        *p = (*p & 0xF0000000u) | (val & 0x0FFFFFFFu);
        if (wrsec(lba, sec) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_alloc(void) {
    uint32_t total = g_bpb.sectors_per_fat * FAT32_SECTOR / 4;
    for (uint32_t c = 2; c < total; c++) {
        if (fat_get(c) == 0) {
            if (fat_set(c, 0x0FFFFFF8u) != 0) return 0;
            /* zero cluster */
            uint8_t zero[FAT32_SECTOR];
            memset(zero, 0, sizeof(zero));
            for (uint32_t s = 0; s < g_bpb.sectors_per_cluster; s++)
                if (wrsec(clus_lba(c)+s, zero) != 0) return 0;
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t first) {
    uint32_t c = first;
    while (c >= 2 && c < 0x0FFFFFF7u) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if (next == c) break;
        c = next;
    }
}

static uint32_t fat_nth(uint32_t first, uint32_t n, uint32_t* prev) {
    uint32_t c = first;
    uint32_t p = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (c < 2 || c >= 0x0FFFFFF7u) return 0;
        p = c; c = fat_get(c);
    }
    if (prev) *prev = p;
    return c;
}

static int fat32_read_file(uint32_t first, uint32_t size, uint32_t off, void* buf, size_t count) {
    if (off >= size || count == 0) return 0;
    if (off + count > size) count = size - off;
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint32_t clus_idx = off / clus_size;
    uint32_t clus_off = off % clus_size;
    uint32_t c = fat_nth(first, clus_idx, 0);
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    uint8_t clbuf[4096]; /* assumes cluster <= 4K; common case 4K */
    if (clus_size > sizeof(clbuf)) return -1;
    while (done < count && c >= 2 && c < 0x0FFFFFF7u) {
        if (rdclus(c, clbuf) != 0) return -1;
        size_t n = clus_size - clus_off;
        if (n > count - done) n = count - done;
        memcpy(out + done, clbuf + clus_off, n);
        done += n; clus_off = 0;
        c = fat_get(c);
    }
    return (int)done;
}

static int fat32_write_file(uint32_t* first, uint32_t* size, uint32_t off, const void* buf, size_t count, int is_dir) {
    (void)is_dir;
    if (count == 0) return 0;
    uint32_t end = off + (uint32_t)count;
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096];
    if (clus_size > sizeof(clbuf)) return -1;

    /* ensure first cluster */
    if (*first < 2) {
        uint32_t nc = fat_alloc(); if (!nc) return -1;
        *first = nc;
    }
    /* extend chain */
    uint32_t need = (end + clus_size - 1) / clus_size;
    uint32_t c = *first; uint32_t last = c; uint32_t have = 1;
    while (c >= 2 && c < 0x0FFFFFF7u) { last = c; c = fat_get(c); have++; }
    have--; /* last iteration counted extra */
    for (uint32_t i = have; i < need; i++) {
        uint32_t nc = fat_alloc(); if (!nc) return -1;
        fat_set(last, nc); last = nc;
    }

    uint32_t clus_idx = off / clus_size;
    uint32_t clus_off = off % clus_size;
    c = fat_nth(*first, clus_idx, 0);
    const uint8_t* in = (const uint8_t*)buf;
    size_t done = 0;
    while (done < count && c >= 2 && c < 0x0FFFFFF7u) {
        if (rdclus(c, clbuf) != 0) return -1;
        size_t n = clus_size - clus_off;
        if (n > count - done) n = count - done;
        memcpy(clbuf + clus_off, in + done, n);
        if (wrclus(c, clbuf) != 0) return -1;
        done += n; clus_off = 0;
        c = fat_get(c);
    }
    if (end > *size) *size = end;
    return (int)done;
}

struct fat_dirent_raw {
    char name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LFN       0x0F

static void fat32_make_sfn(const char* name, char out[11]) {
    for (int i=0;i<11;i++) out[i]=' ';
    int i=0,j=0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c=name[i++]; if(c>='a'&&c<='z') c-=32; out[j++]=c;
    }
    while (name[i] && name[i] != '.') i++;
    if (name[i]=='.') { i++; int k=8; while(name[i]&&k<11){ char c=name[i++]; if(c>='a'&&c<='z') c-=32; out[k++]=c; } }
}

static void fat32_sfn_to_name(const char out11[11], char* name, size_t nsz) {
    size_t j=0;
    for(int i=0;i<8 && j+1<nsz;i++){ char c=out11[i]; if(c==' ') break; if(c>='A'&&c<='Z') c+=32; name[j++]=c; }
    int has_ext=0; for(int i=8;i<11;i++) if(out11[i]!=' ') has_ext=1;
    if(has_ext && j+1<nsz){ name[j++]='.'; for(int i=8;i<11 && j+1<nsz;i++){ char c=out11[i]; if(c==' ') break; if(c>='A'&&c<='Z') c+=32; name[j++]=c; } }
    name[j]='\0';
}

static uint8_t fat32_lfn_checksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static void fat32_lfn_reset(char* buf, size_t sz, uint8_t* chk, int* ok) {
    if (sz) memset(buf, 0, sz);
    *chk = 0; *ok = 0;
}

static void fat32_lfn_push(char* buf, size_t sz, const uint8_t* ent, uint8_t* chk, int* ok) {
    uint8_t seq = ent[0];
    int ord = (seq & 0x3F) - 1;
    if (ord < 0 || ord > 19) { *ok = 0; return; }
    if (seq & 0x40) { memset(buf, 0, sz); *ok = 1; *chk = ent[13]; }
    if (*ok && ent[13] != *chk) { *ok = 0; return; }
    uint16_t chars[13]; int p = 0;
    for (int i=1;i<11;i+=2) chars[p++] = (uint16_t)ent[i] | ((uint16_t)ent[i+1] << 8);
    for (int i=14;i<26;i+=2) chars[p++] = (uint16_t)ent[i] | ((uint16_t)ent[i+1] << 8);
    for (int i=28;i<32;i+=2) chars[p++] = (uint16_t)ent[i] | ((uint16_t)ent[i+1] << 8);
    size_t base = (size_t)ord * 13;
    for (int i=0;i<13;i++) {
        size_t pos = base + (size_t)i;
        if (pos + 1 >= sz) break;
        uint16_t wc = chars[i];
        if (wc == 0x0000 || wc == 0xFFFF) { buf[pos] = '\0'; *ok = *ok ? 1 : 0; return; }
        buf[pos] = (wc < 0x80 && wc >= 0x20) ? (char)wc : '?';
    }
}

static int fat32_dir_find(uint32_t dir_clus, const char* name, struct fat_dirent_raw* out, uint32_t* out_clus, uint32_t* out_idx) {
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    char sfn[11]; fat32_make_sfn(name, sfn);
    char lfn[FAT32_MAX_NAME]; uint8_t lfn_chk = 0; int lfn_ok = 0;
    fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok);
    uint32_t c = dir_clus;
    while (c >= 2 && c < 0x0FFFFFF7u) {
        if (rdclus(c, clbuf) != 0) return -1;
        uint32_t entries = clus_size / 32;
        for (uint32_t i=0;i<entries;i++) {
            struct fat_dirent_raw* e = (struct fat_dirent_raw*)(clbuf + i*32);
            if ((uint8_t)e->name[0] == 0x00) return -1;
            if ((uint8_t)e->name[0] == 0xE5) { fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok); continue; }
            if (e->attr == ATTR_LFN) { fat32_lfn_push(lfn, sizeof(lfn), (const uint8_t*)e, &lfn_chk, &lfn_ok); continue; }
            if (e->attr & ATTR_VOLUME_ID) { fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok); continue; }
            char ename[FAT32_MAX_NAME]; fat32_sfn_to_name(e->name, ename, sizeof(ename));
            int sfn_match = 1; for (int k=0;k<11;k++) if (e->name[k]!=sfn[k]) { sfn_match=0; break; }
            int lfn_match = 0;
            if (lfn_ok && lfn_chk == fat32_lfn_checksum((const uint8_t*)e->name) && lfn[0]) {
                lfn_match = streq(lfn, name);
            }
            if (lfn_match || streq(ename, name) || sfn_match) {
                if (out) *out = *e;
                if (out_clus) *out_clus = c;
                if (out_idx) *out_idx = i;
                return 0;
            }
            fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok);
        }
        c = fat_get(c);
    }
    return -1;
}

static int fat32_dir_alloc_entry(uint32_t dir_clus, uint32_t* out_clus, uint32_t* out_idx) {
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    uint32_t c = dir_clus, last = c;
    while (c >= 2 && c < 0x0FFFFFF7u) {
        if (rdclus(c, clbuf) != 0) return -1;
        uint32_t entries = clus_size / 32;
        for (uint32_t i=0;i<entries;i++) {
            uint8_t b = clbuf[i*32];
            if (b == 0x00 || b == 0xE5) { *out_clus=c; *out_idx=i; return 0; }
        }
        last = c; c = fat_get(c);
    }
    uint32_t nc = fat_alloc(); if (!nc) return -1;
    fat_set(last, nc);
    memset(clbuf, 0, clus_size);
    if (wrclus(nc, clbuf) != 0) return -1;
    *out_clus = nc; *out_idx = 0;
    return 0;
}

static int fat32_dir_write_entry(uint32_t clus, uint32_t idx, const struct fat_dirent_raw* e) {
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    if (rdclus(clus, clbuf) != 0) return -1;
    memcpy(clbuf + idx*32, e, 32);
    return wrclus(clus, clbuf);
}

static int fat32_dir_alloc_entries(uint32_t dir_clus, int n, uint32_t* out_clus, uint32_t* out_idx) {
    if (n <= 0) return -1;
    if (n == 1) return fat32_dir_alloc_entry(dir_clus, out_clus, out_idx);
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint32_t entries_per = clus_size / 32;
    if ((uint32_t)n > entries_per) return -1; /* keep simple */
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    uint32_t c = dir_clus, last = c;
    while (c >= 2 && c < 0x0FFFFFF7u) {
        if (rdclus(c, clbuf) != 0) return -1;
        for (uint32_t i = 0; i + (uint32_t)n <= entries_per; i++) {
            int ok = 1;
            for (int k = 0; k < n; k++) { uint8_t b = clbuf[(i+k)*32]; if (b != 0x00 && b != 0xE5) { ok = 0; i += k; break; } }
            if (ok) { *out_clus = c; *out_idx = i; return 0; }
        }
        last = c; c = fat_get(c);
    }
    uint32_t nc = fat_alloc(); if (!nc) return -1;
    fat_set(last, nc);
    memset(clbuf, 0, clus_size);
    if (wrclus(nc, clbuf) != 0) return -1;
    *out_clus = nc; *out_idx = 0;
    return 0;
}

static int fat32_name_needs_lfn(const char* name) {
    size_t len = 0; int dots = 0; int dotpos = -1;
    for (const char* p = name; *p; p++, len++) { if (*p == '.') { dots++; dotpos = (int)len; } }
    if (len == 0 || len > 255) return 1;
    if (dots > 1) return 1;
    if (dotpos >= 0) {
        int base = dotpos; int ext = (int)len - dotpos - 1;
        if (base > 8 || ext > 3) return 1;
    } else {
        if ((int)len > 8) return 1;
    }
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') return 1; /* lower-case forces LFN in our simple policy */
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') return 1;
    }
    return 0;
}

static void fat32_make_lfn_alias(const char* name, int n, char out[11]) {
    for (int i=0;i<11;i++) out[i]=' ';
    /* base */
    int bi = 0;
    for (const char* p = name; *p && *p != '.' && bi < 6; p++) {
        char c = *p; if (c==' ' || c=='.') continue;
        if (c>='a'&&c<='z') c-=32;
        if ((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-') out[bi++]=c; else out[bi++]='_';
    }
    if (bi == 0) out[bi++]='_';
    out[bi++]='~';
    char num = (char)('0' + (n % 10)); out[bi++]=num;
    /* ext */
    const char* dot = 0; for (const char* p=name; *p; p++) if (*p=='.') dot=p;
    if (dot) {
        int ei=8;
        for (const char* p=dot+1; *p && ei<11; p++) {
            char c=*p; if(c>='a'&&c<='z') c-=32;
            if ((c>='A'&&c<='Z')||(c>='0'&&c<='9')) out[ei++]=c;
        }
    }
}

static int fat32_dir_write_lfn_entries(uint32_t clus, uint32_t idx, const char* name, const char sfn[11]) {
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    if (rdclus(clus, clbuf) != 0) return -1;
    size_t namelen = 0; while (name[namelen]) namelen++;
    int lfn_count = (int)((namelen + 12) / 13);
    uint8_t chk = fat32_lfn_checksum((const uint8_t*)sfn);
    for (int i = lfn_count; i >= 1; i--) {
        uint8_t ent[32]; memset(ent, 0xFF, sizeof(ent));
        ent[0] = (uint8_t)i; if (i == lfn_count) ent[0] |= 0x40;
        ent[11] = ATTR_LFN; ent[12] = 0; ent[13] = chk; ent[26] = 0; ent[27] = 0;
        size_t base = (size_t)(i-1) * 13;
        uint16_t chars[13];
        for (int j=0;j<13;j++) {
            size_t pos = base + (size_t)j;
            if (pos < namelen) chars[j] = (uint8_t)name[pos];
            else if (pos == namelen) chars[j] = 0x0000;
            else chars[j] = 0xFFFF;
        }
        int p=0;
        for (int j=1;j<11;j+=2) { ent[j]=(uint8_t)(chars[p]&0xFF); ent[j+1]=(uint8_t)(chars[p]>>8); p++; }
        for (int j=14;j<26;j+=2) { ent[j]=(uint8_t)(chars[p]&0xFF); ent[j+1]=(uint8_t)(chars[p]>>8); p++; }
        for (int j=28;j<32;j+=2) { ent[j]=(uint8_t)(chars[p]&0xFF); ent[j+1]=(uint8_t)(chars[p]>>8); p++; }
        uint32_t eidx = idx + (uint32_t)(lfn_count - i);
        memcpy(clbuf + eidx*32, ent, 32);
    }
    return wrclus(clus, clbuf);
}

static uint32_t fat32_entry_clus(const struct fat_dirent_raw* e) {
    return ((uint32_t)e->fst_clus_hi << 16) | e->fst_clus_lo;
}

static void fat32_set_entry_clus(struct fat_dirent_raw* e, uint32_t c) {
    e->fst_clus_hi = (uint16_t)(c >> 16);
    e->fst_clus_lo = (uint16_t)(c & 0xFFFFu);
}

static int fat32_path_parent(const char* path, char* parent, size_t psz, char* name, size_t nsz) {
    char norm[FAT32_MAX_NAME]; strcopy(norm, path, sizeof(norm));
    /* strip trailing / */
    size_t len=0; while(norm[len]) len++;
    while (len>1 && is_sep(norm[len-1])) norm[--len]='\0';
    /* find last sep */
    int last=-1; for(int i=(int)len-1;i>=0;i--) if(is_sep(norm[i])) { last=i; break; }
    if (last < 0) { strcopy(parent, "/", psz); strcopy(name, norm, nsz); return 0; }
    if (last==0) { strcopy(parent, "/", psz); } else { size_t k=0; for(int i=0;i<last && k+1<psz;i++) parent[k++]=norm[i]; parent[k]='\0'; }
    strcopy(name, norm+last+1, nsz);
    return 0;
}

static uint32_t fat32_resolve_dir(const char* path) {
    if (!path || streq(path, "/") || streq(path, "")) return g_bpb.root_cluster;
    char p[FAT32_MAX_NAME]; strcopy(p, path, sizeof(p));
    uint32_t cur = g_bpb.root_cluster;
    char* s = p;
    while (*s) {
        while (is_sep(*s)) s++;
        if (!*s) break;
        char comp[FAT32_MAX_NAME]; size_t j=0;
        while (*s && !is_sep(*s) && j+1<sizeof(comp)) comp[j++]=*s++;
        comp[j]='\0';
        struct fat_dirent_raw e;
        if (fat32_dir_find(cur, comp, &e, 0, 0) != 0) return 0;
        if (!(e.attr & ATTR_DIRECTORY)) return 0;
        cur = fat32_entry_clus(&e);
        if (cur == 0) cur = g_bpb.root_cluster;
    }
    return cur;
}

static int fat32_slot_alloc(void) {
    for (int i=0;i<FAT32_MAX_SLOTS;i++) if (!g_slots[i].used) return i;
    return -1;
}

static int fat32_op_stat(void* ctx, const char* path, struct arc_stat* st);

int fat32_is_mounted(void) { return g_mounted; }

int fat32_unmount(void) {
    g_mounted = 0;
    g_dev = 0;
    memset(g_slots, 0, sizeof(g_slots));
    memset(&g_bpb, 0, sizeof(g_bpb));
    return 0;
}

int fat32_qualifies_arcroot(void) {
    if (!g_mounted || !g_dev) return 0;
    /* Check BPB volume label (FAT32 EBPB offset 71, 11 bytes, space-padded). */
    uint8_t sec[FAT32_SECTOR];
    if (blockdev_read(g_dev, g_bpb.part_lba, 1, sec) == 0) {
        const char *want = "ARCROOT";
        int ok = 1;
        for (int i = 0; i < 7; i++) {
            if (sec[71 + i] != (uint8_t)want[i]) { ok = 0; break; }
        }
        for (int i = 7; i < 11; i++) {
            if (sec[71 + i] != (uint8_t)' ') { ok = 0; break; }
        }
        if (ok) return 1;
    }
    /* Check marker file /ARCROOT.MRK */
    struct arc_stat st;
    if (fat32_op_stat(0, "/ARCROOT.MRK", &st) == 0 && !st.is_dir) return 1;
    return 0;
}

static int fat32_is_readonly(void) { return g_dev && g_dev->read_only; }

static int fat32_op_open(void* ctx, const char* path, int create, int* out_slot) {
    (void)ctx;
    if (!g_mounted) return -1;
    char parent[FAT32_MAX_NAME], name[FAT32_MAX_NAME];
    fat32_path_parent(path, parent, sizeof(parent), name, sizeof(name));
    if (name[0]=='\0') return -1;
    /* check existing */
    for (int i=0;i<FAT32_MAX_SLOTS;i++) {
        if (g_slots[i].used && !g_slots[i].deleted && streq(g_slots[i].path, path)) { *out_slot=i; return 0; }
    }
    uint32_t dir = fat32_resolve_dir(parent);
    if (dir == 0) return -1;
    struct fat_dirent_raw e; uint32_t eclus, eidx;
    int found = (fat32_dir_find(dir, name, &e, &eclus, &eidx) == 0);
    if (!found && !create) return -1;
    if (!found && create && fat32_is_readonly()) return -1;
    int slot = fat32_slot_alloc(); if (slot < 0) return -1;
    struct fat32_slot* s = &g_slots[slot];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    strcopy(s->path, path, sizeof(s->path));
    if (found) {
        s->dir_cluster = eclus; s->dir_index = eidx;
        s->first_cluster = fat32_entry_clus(&e);
        s->size = e.file_size;
        s->is_dir = (e.attr & ATTR_DIRECTORY) ? 1 : 0;
    } else {
        uint32_t nc, ni;
        struct fat_dirent_raw ne; memset(&ne, 0, sizeof(ne));
        ne.attr = 0x20; /* archive */
        if (fat32_name_needs_lfn(name)) {
            size_t namelen = 0; while (name[namelen]) namelen++;
            int lfn_count = (int)((namelen + 12) / 13);
            int alias_n = 1; char alias[11];
            for (;;) {
                fat32_make_lfn_alias(name, alias_n, alias);
                struct fat_dirent_raw tmp; uint32_t tc,ti;
                /* check alias collision by SFN match */
                int coll = 0;
                {
                    uint32_t cc = dir; uint32_t csz=g_bpb.sectors_per_cluster*FAT32_SECTOR; uint8_t cbuf[4096];
                    while (cc>=2 && cc<0x0FFFFFF7u && !coll) {
                        if (rdclus(cc,cbuf)!=0) break;
                        uint32_t ents=csz/32;
                        for (uint32_t ei2=0; ei2<ents; ei2++) {
                            struct fat_dirent_raw* ee=(struct fat_dirent_raw*)(cbuf+ei2*32);
                            if ((uint8_t)ee->name[0]==0x00) { cc=0; break; }
                            if ((uint8_t)ee->name[0]==0xE5 || ee->attr==ATTR_LFN) continue;
                            int sm=1; for(int k=0;k<11;k++) if(ee->name[k]!=alias[k]){sm=0;break;}
                            if (sm) { coll=1; break; }
                        }
                        if (cc==0) break;
                        cc=fat_get(cc);
                    }
                }
                (void)tmp;(void)tc;(void)ti;
                if (!coll || alias_n >= 9) { memcpy(ne.name, alias, 11); break; }
                alias_n++;
            }
            if (fat32_dir_alloc_entries(dir, lfn_count + 1, &nc, &ni) != 0) return -1;
            if (fat32_dir_write_lfn_entries(nc, ni, name, (const char*)ne.name) != 0) return -1;
            if (fat32_dir_write_entry(nc, ni + (uint32_t)lfn_count, &ne) != 0) return -1;
            s->dir_cluster = nc; s->dir_index = ni + (uint32_t)lfn_count;
        } else {
            if (fat32_dir_alloc_entry(dir, &nc, &ni) != 0) return -1;
            fat32_make_sfn(name, ne.name);
            if (fat32_dir_write_entry(nc, ni, &ne) != 0) return -1;
            s->dir_cluster = nc; s->dir_index = ni;
        }
        s->first_cluster = 0; s->size = 0; s->is_dir = 0;
    }
    *out_slot = slot;
    return 0;
}

static int fat32_flush_dirent(struct fat32_slot* s) {
    struct fat_dirent_raw e;
    uint32_t clus_size = g_bpb.sectors_per_cluster * FAT32_SECTOR;
    uint8_t clbuf[4096]; if (clus_size > sizeof(clbuf)) return -1;
    if (rdclus(s->dir_cluster, clbuf) != 0) return -1;
    memcpy(&e, clbuf + s->dir_index*32, 32);
    e.file_size = s->size;
    fat32_set_entry_clus(&e, s->first_cluster);
    memcpy(clbuf + s->dir_index*32, &e, 32);
    return wrclus(s->dir_cluster, clbuf);
}

static int fat32_op_read(void* ctx, int slot, uint32_t off, void* buf, size_t count) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return -1;
    struct fat32_slot* s=&g_slots[slot];
    if (s->is_dir) return -1;
    return fat32_read_file(s->first_cluster, s->size, off, buf, count);
}

static int fat32_op_write(void* ctx, int slot, uint32_t off, const void* buf, size_t count) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return -1;
    if (fat32_is_readonly()) return -1;
    struct fat32_slot* s=&g_slots[slot];
    if (s->is_dir) return -1;
    int n = fat32_write_file(&s->first_cluster, &s->size, off, buf, count, 0);
    if (n >= 0) fat32_flush_dirent(s);
    return n;
}

static int fat32_op_size(void* ctx, int slot) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return -1;
    return (int)g_slots[slot].size;
}

static int fat32_op_truncate(void* ctx, int slot) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return -1;
    if (fat32_is_readonly()) return -1;
    struct fat32_slot* s=&g_slots[slot];
    if (s->first_cluster >= 2) fat_free_chain(s->first_cluster);
    s->first_cluster = 0; s->size = 0;
    fat32_flush_dirent(s);
    return 0;
}

static int fat32_op_add_ref(void* ctx, int slot) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return -1;
    if (g_slots[slot].deleted) return -1;
    g_slots[slot].refs++;
    return 0;
}

static void fat32_op_release(void* ctx, int slot) {
    (void)ctx;
    if (slot<0||slot>=FAT32_MAX_SLOTS||!g_slots[slot].used) return;
    struct fat32_slot* s=&g_slots[slot];
    if (s->refs>0) s->refs--;
    if (s->refs==0) {
        if (s->deleted) { if (s->first_cluster>=2) fat_free_chain(s->first_cluster); }
        memset(s,0,sizeof(*s));
    }
}

static int fat32_op_close_slot(void* ctx, int slot) { fat32_op_release(ctx,slot); return 0; }

static int fat32_op_stat(void* ctx, const char* path, struct arc_stat* st) {
    (void)ctx; if(!g_mounted||!st) return -1;
    if (streq(path,"/") || streq(path,"")) { st->size=0; st->is_dir=1; st->attrs=0; return 0; }
    char parent[FAT32_MAX_NAME], name[FAT32_MAX_NAME];
    fat32_path_parent(path,parent,sizeof(parent),name,sizeof(name));
    uint32_t dir = fat32_resolve_dir(parent); if (dir==0) return -1;
    struct fat_dirent_raw e;
    if (fat32_dir_find(dir,name,&e,0,0)!=0) return -1;
    st->size=e.file_size; st->is_dir=(e.attr&ATTR_DIRECTORY)?1:0; st->attrs=e.attr;
    return 0;
}

static int fat32_op_readdir(void* ctx, const char* path, uint32_t index, struct arc_dirent* out) {
    (void)ctx; if(!g_mounted||!out) return -1;
    uint32_t dir = fat32_resolve_dir(path); if (dir==0) return -1;
    uint32_t clus_size=g_bpb.sectors_per_cluster*FAT32_SECTOR;
    uint8_t clbuf[4096]; if(clus_size>sizeof(clbuf)) return -1;
    char lfn[FAT32_MAX_NAME]; uint8_t lfn_chk = 0; int lfn_ok = 0;
    fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok);
    uint32_t seen=0; uint32_t c=dir;
    while (c>=2 && c<0x0FFFFFF7u) {
        if (rdclus(c,clbuf)!=0) return -1;
        uint32_t entries=clus_size/32;
        for (uint32_t i=0;i<entries;i++) {
            struct fat_dirent_raw* e=(struct fat_dirent_raw*)(clbuf+i*32);
            if ((uint8_t)e->name[0]==0x00) return -1;
            if ((uint8_t)e->name[0]==0xE5) { fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok); continue; }
            if (e->attr==ATTR_LFN) { fat32_lfn_push(lfn, sizeof(lfn), (const uint8_t*)e, &lfn_chk, &lfn_ok); continue; }
            if (e->attr&ATTR_VOLUME_ID) { fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok); continue; }
            if (e->name[0]=='.') { fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok); continue; }
            if (seen==index) {
                if (lfn_ok && lfn_chk == fat32_lfn_checksum((const uint8_t*)e->name) && lfn[0]) {
                    size_t n = 0; while (lfn[n] && n < sizeof(out->name)-1) { out->name[n]=lfn[n]; n++; } out->name[n]='\0';
                } else {
                    fat32_sfn_to_name(e->name, out->name, sizeof(out->name));
                }
                out->size=e->file_size; out->is_dir=(e->attr&ATTR_DIRECTORY)?1:0;
                return 0;
            }
            seen++;
            fat32_lfn_reset(lfn, sizeof(lfn), &lfn_chk, &lfn_ok);
        }
        c=fat_get(c);
    }
    return -1;
}

static int fat32_op_mkdir(void* ctx, const char* path) {
    (void)ctx; if(!g_mounted) return -1;
    if (fat32_is_readonly()) return -1;
    char parent[FAT32_MAX_NAME], name[FAT32_MAX_NAME];
    fat32_path_parent(path,parent,sizeof(parent),name,sizeof(name));
    uint32_t dir = fat32_resolve_dir(parent); if (dir==0) return -1;
    struct fat_dirent_raw e; if (fat32_dir_find(dir,name,&e,0,0)==0) return -1;
    uint32_t c = fat_alloc(); if(!c) return -1;
    struct fat_dirent_raw ne; memset(&ne,0,sizeof(ne));
    ne.attr=ATTR_DIRECTORY; fat32_set_entry_clus(&ne,c);
    uint32_t nc,ni;
    if (fat32_name_needs_lfn(name)) {
        size_t namelen=0; while(name[namelen]) namelen++;
        int lfn_count=(int)((namelen+12)/13);
        int alias_n=1; char alias[11];
        for (;;) {
            fat32_make_lfn_alias(name,alias_n,alias);
            int coll=0; uint32_t cc=dir; uint32_t csz=g_bpb.sectors_per_cluster*FAT32_SECTOR; uint8_t cbuf2[4096];
            while(cc>=2&&cc<0x0FFFFFF7u&&!coll){ if(rdclus(cc,cbuf2)!=0)break; uint32_t ents=csz/32; for(uint32_t ei2=0;ei2<ents;ei2++){ struct fat_dirent_raw* ee=(struct fat_dirent_raw*)(cbuf2+ei2*32); if((uint8_t)ee->name[0]==0x00){cc=0;break;} if((uint8_t)ee->name[0]==0xE5||ee->attr==ATTR_LFN)continue; int sm=1; for(int k=0;k<11;k++) if(ee->name[k]!=alias[k]){sm=0;break;} if(sm){coll=1;break;} } if(cc==0)break; cc=fat_get(cc); }
            if(!coll||alias_n>=9){ memcpy(ne.name,alias,11); break; }
            alias_n++;
        }
        if (fat32_dir_alloc_entries(dir, lfn_count+1, &nc,&ni)!=0) return -1;
        if (fat32_dir_write_lfn_entries(nc,ni,name,(const char*)ne.name)!=0) return -1;
        if (fat32_dir_write_entry(nc,ni+(uint32_t)lfn_count,&ne)!=0) return -1;
    } else {
        if (fat32_dir_alloc_entry(dir,&nc,&ni)!=0) return -1;
        fat32_make_sfn(name, ne.name);
        if (fat32_dir_write_entry(nc,ni,&ne)!=0) return -1;
    }
    /* . and .. */
    uint32_t clus_size=g_bpb.sectors_per_cluster*FAT32_SECTOR;
    uint8_t clbuf[4096]; memset(clbuf,0,clus_size);
    struct fat_dirent_raw dot,dotdot; memset(&dot,0,sizeof(dot)); memset(&dotdot,0,sizeof(dotdot));
    for(int i=0;i<11;i++){dot.name[i]=' ';dotdot.name[i]=' ';} dot.name[0]='.'; dotdot.name[0]='.'; dotdot.name[1]='.';
    dot.attr=ATTR_DIRECTORY; dotdot.attr=ATTR_DIRECTORY;
    fat32_set_entry_clus(&dot,c); fat32_set_entry_clus(&dotdot, dir==g_bpb.root_cluster?0:dir);
    memcpy(clbuf,&dot,32); memcpy(clbuf+32,&dotdot,32);
    wrclus(c,clbuf);
    return 0;
}

static int fat32_op_unlink(void* ctx, const char* path) {
    (void)ctx; if(!g_mounted) return -1;
    if (fat32_is_readonly()) return -1;
    /* check open slots */
    for(int i=0;i<FAT32_MAX_SLOTS;i++) {
        if(g_slots[i].used && streq(g_slots[i].path,path)) {
            struct fat32_slot* s=&g_slots[i];
            s->deleted=1;
            /* remove dir entry so name can't be reopened */
            char parent[FAT32_MAX_NAME], name[FAT32_MAX_NAME];
            fat32_path_parent(path,parent,sizeof(parent),name,sizeof(name));
            uint32_t dir = fat32_resolve_dir(parent);
            if (dir != 0) {
                struct fat_dirent_raw e; uint32_t ec,ei;
                if (fat32_dir_find(dir,name,&e,&ec,&ei)==0 && !(e.attr & ATTR_DIRECTORY)) {
                    e.name[0] = (char)0xE5;
                    fat32_dir_write_entry(ec,ei,&e);
                }
            }
            if (s->refs == 0) {
                if (s->first_cluster >= 2) fat_free_chain(s->first_cluster);
                memset(s,0,sizeof(*s));
            }
            return 0;
        }
    }
    char parent[FAT32_MAX_NAME], name[FAT32_MAX_NAME];
    fat32_path_parent(path,parent,sizeof(parent),name,sizeof(name));
    uint32_t dir = fat32_resolve_dir(parent); if (dir==0) return -1;
    struct fat_dirent_raw e; uint32_t ec,ei;
    if (fat32_dir_find(dir,name,&e,&ec,&ei)!=0) return -1;
    if (e.attr & ATTR_DIRECTORY) return -1; /* simple: no rmdir for now */
    uint32_t first = fat32_entry_clus(&e);
    e.name[0] = (char)0xE5;
    fat32_dir_write_entry(ec,ei,&e);
    if (first >= 2) fat_free_chain(first);
    return 0;
}

static int fat32_op_rename(void* ctx, const char* oldp, const char* newp) {
    (void)ctx; if(!g_mounted) return -1;
    if (fat32_is_readonly()) return -1;
    /* simple: read, create new, delete old */
    struct arc_stat st; if (fat32_op_stat(0,oldp,&st)!=0) return -1;
    if (st.is_dir) return -1;
    int slot=-1; if (fat32_op_open(0,oldp,0,&slot)!=0) return -1;
    if (slot < 0 || slot >= FAT32_MAX_SLOTS) return -1;
    fat32_op_add_ref(0,slot);
    uint32_t sz=g_slots[slot].size;
    uint8_t* buf = (uint8_t*)kmalloc(sz?sz:1); if(!buf){fat32_op_release(0,slot);return -1;}
    if(sz) fat32_op_read(0,slot,0,buf,sz);
    fat32_op_release(0,slot);
    int ns=-1; if (fat32_op_open(0,newp,1,&ns)!=0){kfree(buf);return -1;}
    fat32_op_add_ref(0,ns);
    fat32_op_truncate(0,ns);
    if(sz) fat32_op_write(0,ns,0,buf,sz);
    fat32_op_release(0,ns); kfree(buf);
    return fat32_op_unlink(0,oldp);
}

static int fat32_op_sync(void* ctx) {
    (void)ctx;
    if (!g_mounted || !g_dev) return -1;
    /* FAT32 metadata writes are write-through in this driver; nothing to
     * write back here. vfs_sync() performs the blockdev flush after the
     * FS sync hook returns, giving the durability boundary. */
    return 0;
}

static const struct vfs_ops g_fat32_ops = {
    fat32_op_open, fat32_op_read, fat32_op_write, fat32_op_size, fat32_op_truncate,
    fat32_op_close_slot, fat32_op_add_ref, fat32_op_release,
    fat32_op_stat, fat32_op_readdir, fat32_op_mkdir, fat32_op_unlink, fat32_op_rename,
    fat32_op_sync
};

int fat32_mount(struct blockdev* dev, uint32_t lba_start, uint32_t sector_count) {
    (void)sector_count;
    uint8_t sec[FAT32_SECTOR];
    if (blockdev_read(dev, lba_start, 1, sec) != 0) return -1;
    /* check BPB signature */
    if (sec[510]!=0x55 || sec[511]!=0xAA) return -1;
    uint16_t bps = *(uint16_t*)(sec+11);
    uint8_t spc = sec[13];
    uint16_t rsv = *(uint16_t*)(sec+14);
    uint8_t nf = sec[16];
    uint32_t spf = *(uint32_t*)(sec+36);
    uint32_t root = *(uint32_t*)(sec+44);
    uint32_t tot16 = *(uint16_t*)(sec+19);
    uint32_t tot32 = *(uint32_t*)(sec+32);
    uint32_t tot = tot16 ? tot16 : tot32;
    if (bps != 512 || spc == 0 || nf == 0 || spf == 0) return -1;

    memset(&g_bpb, 0, sizeof(g_bpb));
    g_bpb.bytes_per_sector=bps; g_bpb.sectors_per_cluster=spc; g_bpb.reserved_sectors=rsv;
    g_bpb.fat_count=nf; g_bpb.sectors_per_fat=spf; g_bpb.root_cluster=root;
    g_bpb.total_sectors=tot; g_bpb.part_lba=lba_start;
    g_bpb.fat_start_lba=rsv;
    g_bpb.data_start_lba=rsv + nf*spf;
    g_dev=dev; g_mounted=1;
    memset(g_slots,0,sizeof(g_slots));
    vfs_mount("/", &g_fat32_ops, 0, dev);
    terminal_putstring("FAT32: mounted\n");
    return 0;
}

/* ---- fs_slot bridge for VFS/FAT32 dispatch ---- */
int fat32_open_slot(const char* name, int create) {
    int slot = -1;
    if (fat32_op_open(0, name, create, &slot) != 0) return -1;
    return slot;
}
int fat32_slot_read(int slot, uint32_t off, void* buf, size_t count) {
    return fat32_op_read(0, slot, off, buf, count);
}
int fat32_slot_write(int slot, uint32_t off, const void* buf, size_t count) {
    return fat32_op_write(0, slot, off, buf, count);
}
int fat32_slot_size(int slot) { return fat32_op_size(0, slot); }
int fat32_slot_truncate(int slot) { return fat32_op_truncate(0, slot); }
int fat32_slot_add_ref(int slot) { return fat32_op_add_ref(0, slot); }
void fat32_slot_release(int slot) { fat32_op_release(0, slot); }
int fat32_delete(const char* name) { return fat32_op_unlink(0, name); }
int fat32_rename(const char* oldname, const char* newname) { return fat32_op_rename(0, oldname, newname); }
int fat32_list(void) {
    terminal_putstring("Files:\n");
    struct arc_dirent de;
    for (uint32_t i = 0; ; i++) {
        if (fat32_op_readdir(0, "/", i, &de) != 0) break;
        terminal_putstring("  ");
        terminal_putstring(de.name);
        if (de.is_dir) { terminal_putstring("/\n"); }
        else { terminal_putstring("  ("); terminal_print_int(de.size); terminal_putstring(" bytes)\n"); }
    }
    return 0;
}

int fat32_stat(const char* path, struct arc_stat* st) { return fat32_op_stat(0, path, st); }
int fat32_readdir(const char* path, uint32_t index, struct arc_dirent* out) { return fat32_op_readdir(0, path, index, out); }
int fat32_mkdir(const char* path) { return fat32_op_mkdir(0, path); }
