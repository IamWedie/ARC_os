/*
 * Host-side stubs for the real filesystem slot tests.
 *
 * kernel/fs.c is linked directly into this suite, so these stubs stand in for
 * the pieces it borrows from the kernel: the heap, the ATA disk, and the
 * console. Physical frames come from tests/pmm_stub.c.
 *
 * The fake disk defaults to absent (RAM-only filesystem). A test that needs a
 * mounted image calls fs_stub_disk_reset(1) and writes sectors with
 * fs_stub_disk_write_bytes() before fs_init().
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

#define FS_STUB_DISK_SECTORS 2048u

static uint8_t fs_stub_disk[FS_STUB_DISK_SECTORS * FS_SECTOR_SIZE];
static int fs_stub_disk_present;
static char fs_stub_console[2048];
static size_t fs_stub_console_len;

/* --- heap --- */

void* kmalloc(size_t size) {
    return malloc(size);
}

void kfree(void* ptr) {
    free(ptr);
}

/* --- fake ATA disk --- */

int ata_present(void) {
    return fs_stub_disk_present;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void* buf) {
    if (!buf && count > 0) return -1;
    if ((uint64_t)lba + count > FS_STUB_DISK_SECTORS) return -1;
    memcpy(buf, fs_stub_disk + (size_t)lba * FS_SECTOR_SIZE,
           (size_t)count * FS_SECTOR_SIZE);
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf) {
    if (!buf && count > 0) return -1;
    if ((uint64_t)lba + count > FS_STUB_DISK_SECTORS) return -1;
    memcpy(fs_stub_disk + (size_t)lba * FS_SECTOR_SIZE, buf,
           (size_t)count * FS_SECTOR_SIZE);
    return 0;
}

/* Reset the fake disk and console between tests. */
void fs_stub_disk_reset(int present) {
    memset(fs_stub_disk, 0, sizeof(fs_stub_disk));
    fs_stub_disk_present = present;
    fs_stub_console_len = 0;
    fs_stub_console[0] = '\0';
}

/* Write raw bytes into the fake disk starting at sector `lba`. */
void fs_stub_disk_write_bytes(uint32_t lba, const void* data, size_t len) {
    if (!data) return;
    if ((uint64_t)lba * FS_SECTOR_SIZE + len > sizeof(fs_stub_disk)) return;
    memcpy(fs_stub_disk + (size_t)lba * FS_SECTOR_SIZE, data, len);
}

/* --- console --- */

static void console_append(const char* s) {
    while (*s && fs_stub_console_len < sizeof(fs_stub_console) - 1) {
        fs_stub_console[fs_stub_console_len++] = *s++;
    }
    fs_stub_console[fs_stub_console_len] = '\0';
}

void terminal_putstring(const char* str) {
    console_append(str);
}

void terminal_putchar(char c, uint8_t color) {
    (void)color;
    char s[2] = {c, '\0'};
    console_append(s);
}

void terminal_write(const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char s[2] = {buf[i], '\0'};
        console_append(s);
    }
}

void terminal_print_int(uint64_t n) {
    char buf[32];
    int i = 0;
    if (n == 0) {
        console_append("0");
        return;
    }
    while (n > 0 && i < (int)sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        char s[2] = {buf[--i], '\0'};
        console_append(s);
    }
}

void terminal_print_hex(uint64_t n) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[n & 0xf];
        n >>= 4;
    }
    buf[16] = '\0';
    console_append(buf);
}

const char* fs_stub_console_text(void) {
    return fs_stub_console;
}
