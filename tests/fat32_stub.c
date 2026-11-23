/* Host stubs for tests/test_fat32.c linking kernel/fat32.c */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef SYS_OPEN
#include "kernel.h"

#define STUB_DISK_SECTORS 32768u
static uint8_t g_disk[STUB_DISK_SECTORS * 512];
static char g_console[4096];
static size_t g_con_len;
static int g_write_count;

void* kmalloc(size_t size) { return malloc(size); }
void kfree(void* ptr) { free(ptr); }

void terminal_putstring(const char* s) {
    while (*s && g_con_len < sizeof(g_console)-1) g_console[g_con_len++]=*s++;
    g_console[g_con_len]='\0';
}
void terminal_putchar(char c, uint8_t color) { (void)color; char s[2]={c,0}; terminal_putstring(s); }
void terminal_write(const char* buf, size_t count) { for(size_t i=0;i<count;i++){ char s[2]={buf[i],0}; terminal_putstring(s);} }
void terminal_print_int(uint64_t n) { char b[32]; snprintf(b,sizeof(b),"%llu",(unsigned long long)n); terminal_putstring(b); }
void terminal_print_hex(uint64_t n) { char b[32]; snprintf(b,sizeof(b),"0x%llx",(unsigned long long)n); terminal_putstring(b); }

int blockdev_read(struct blockdev* dev, uint32_t lba, uint32_t count, void* buf) {
    (void)dev;
    if ((uint64_t)lba + count > STUB_DISK_SECTORS) return -1;
    memcpy(buf, g_disk + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}
int blockdev_write(struct blockdev* dev, uint32_t lba, uint32_t count, const void* buf) {
    (void)dev;
    g_write_count++;
    if ((uint64_t)lba + count > STUB_DISK_SECTORS) return -1;
    memcpy(g_disk + (size_t)lba * 512, buf, (size_t)count * 512);
    return 0;
}
int blockdev_flush(struct blockdev* dev) { (void)dev; return 0; }

int vfs_mount(const char* path, const struct vfs_ops* ops, void* ctx, struct blockdev* dev) { (void)path; (void)ops; (void)ctx; (void)dev; return 0; }
int vfs_stat(const char* path, struct arc_stat* st) { (void)path; (void)st; return -1; }
int vfs_readdir(const char* path, uint32_t index, struct arc_dirent* out) { (void)path; (void)index; (void)out; return -1; }
int vfs_mkdir(const char* path) { (void)path; return -1; }
int vfs_unlink(const char* path) { (void)path; return -1; }
int vfs_mount_device(const char* devname, const char* fstype, const char* target) { (void)devname; (void)fstype; (void)target; return -1; }

void fat32_stub_reset(void) { memset(g_disk, 0, sizeof(g_disk)); g_con_len=0; g_console[0]='\0'; g_write_count=0; }
uint8_t* fat32_stub_disk(void) { return g_disk; }
uint32_t fat32_stub_disk_sectors(void) { return STUB_DISK_SECTORS; }
const char* fat32_stub_console(void) { return g_console; }
int fat32_stub_write_count(void) { return g_write_count; }
void fat32_stub_reset_write_count(void) { g_write_count = 0; }
