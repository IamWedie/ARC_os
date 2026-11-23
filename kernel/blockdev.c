/* Block device layer (M4): registration, read/write/flush, small write-through cache. */
#include "kernel.h"

#define BLK_CACHE_SECTORS 32

struct blk_cache_entry {
    struct blockdev* dev;
    uint32_t lba;
    uint8_t valid;
    uint8_t dirty; /* reserved for write-back; write-through keeps 0 */
    uint8_t data[BLOCK_SECTOR_SIZE];
};

static struct blockdev* g_devs[BLOCKDEV_MAX];
static int g_dev_count;
static struct blk_cache_entry g_cache[BLK_CACHE_SECTORS];
static uint32_t g_cache_evict;

int blockdev_register(struct blockdev* dev) {
    if (!dev || g_dev_count >= BLOCKDEV_MAX) return -1;
    for (int i = 0; i < g_dev_count; i++) {
        if (g_devs[i] == dev) return i;
    }
    g_devs[g_dev_count] = dev;
    return g_dev_count++;
}

struct blockdev* blockdev_get(int idx) {
    if (idx < 0 || idx >= g_dev_count) return 0;
    return g_devs[idx];
}

int blockdev_count(void) { return g_dev_count; }

struct blockdev* blockdev_find(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_dev_count; i++) {
        const char* a = g_devs[i]->name;
        const char* b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return g_devs[i];
    }
    return 0;
}

static struct blk_cache_entry* cache_find(struct blockdev* dev, uint32_t lba) {
    for (int i = 0; i < BLK_CACHE_SECTORS; i++) {
        if (g_cache[i].valid && g_cache[i].dev == dev && g_cache[i].lba == lba)
            return &g_cache[i];
    }
    return 0;
}

static struct blk_cache_entry* cache_alloc(struct blockdev* dev, uint32_t lba) {
    struct blk_cache_entry* e = cache_find(dev, lba);
    if (e) return e;
    /* find invalid */
    for (int i = 0; i < BLK_CACHE_SECTORS; i++) {
        if (!g_cache[i].valid) { e = &g_cache[i]; goto init; }
    }
    e = &g_cache[g_cache_evict++ % BLK_CACHE_SECTORS];
    /* write-through: dirty should never be set, but be safe */
    if (e->valid && e->dirty && e->dev && e->dev->write) {
        e->dev->write(e->dev, e->lba, 1, e->data);
    }
init:
    e->dev = dev;
    e->lba = lba;
    e->valid = 1;
    e->dirty = 0;
    return e;
}

int blockdev_read(struct blockdev* dev, uint32_t lba, uint32_t count, void* buf) {
    if (!dev || !dev->read || !buf) return -1;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur = lba + i;
        struct blk_cache_entry* c = cache_find(dev, cur);
        if (c) {
            memcpy(out + i * BLOCK_SECTOR_SIZE, c->data, BLOCK_SECTOR_SIZE);
            continue;
        }
        c = cache_alloc(dev, cur);
        if (dev->read(dev, cur, 1, c->data) != 0) {
            c->valid = 0;
            return -1;
        }
        memcpy(out + i * BLOCK_SECTOR_SIZE, c->data, BLOCK_SECTOR_SIZE);
    }
    return 0;
}

int blockdev_write(struct blockdev* dev, uint32_t lba, uint32_t count, const void* buf) {
    if (!dev || !dev->write || !buf) return -1;
    const uint8_t* in = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur = lba + i;
        if (dev->write(dev, cur, 1, in + i * BLOCK_SECTOR_SIZE) != 0) return -1;
        struct blk_cache_entry* c = cache_alloc(dev, cur);
        memcpy(c->data, in + i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
        c->dirty = 0; /* write-through */
    }
    return 0;
}

int blockdev_flush(struct blockdev* dev) {
    if (!dev) return -1;
    if (dev->flush) return dev->flush(dev);
    return 0;
}