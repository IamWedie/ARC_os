#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef SYS_OPEN
#include "kernel.h"

int part_probe(struct blockdev* dev, struct part_info* out, int max) {
    (void)dev; (void)out; (void)max;
    return 0;
}

int fat32_mount(struct blockdev* dev, uint32_t lba_start, uint32_t sector_count) {
    (void)dev; (void)lba_start; (void)sector_count;
    return -1;
}
