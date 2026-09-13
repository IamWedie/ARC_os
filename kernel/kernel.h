/*
 * TinyOS Kernel Header
 * Updated with keyboard and process management
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BUFFER 0xB8000

#define COLOR_BLACK 0
#define COLOR_BLUE 1
#define COLOR_GREEN 2
#define COLOR_CYAN 3
#define COLOR_RED 4
#define COLOR_MAGENTA 5
#define COLOR_BROWN 6
#define COLOR_LIGHT_GREY 7
#define COLOR_DARK_GREY 8
#define COLOR_LIGHT_BLUE 9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_CYAN 11
#define COLOR_LIGHT_RED 12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_LIGHT_BROWN 14
#define COLOR_WHITE 15

extern uint8_t terminal_color;
extern size_t terminal_row;
extern size_t terminal_col;
extern uint16_t* terminal_buffer;

/* Console */
void terminal_initialize(void);
void terminal_putchar(char c, uint8_t color);
void terminal_putstring(const char* str);
void terminal_setcolor(uint8_t color);
void terminal_clear(void);
void terminal_scroll(void);
void terminal_print_int(uint64_t n);
void terminal_print_hex(uint64_t n);

/* Memory */
void* kmalloc(size_t size);
void kfree(void* ptr);
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int strlen(const char* s);
void* memmove(void* dest, const void* src, size_t n);
int strcmp(const char* a, const char* b);

/* GDT */
void gdt_init(void);

/* IDT */
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

/* Interrupts */
void interrupts_init(void);
void outb(uint16_t port, uint8_t val);
uint8_t inb(uint16_t port);

/* Scheduler */
#define MAX_PROCESSES 16
#define USER_STACK_TOP 0x90000000

typedef struct {
    uint64_t rsp;
    uint64_t rip;
    int pid;
    int running;
    uint64_t stack_top;
    char name[32];
} process_t;

void scheduler_init(void);
void scheduler_yield(void);
void process_create(void (*entry)(void), const char* name);

/* Filesystem */
#define FS_SECTOR_SIZE 512
#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILESIZE 65536

typedef struct {
    char name[FS_MAX_FILENAME];
    uint32_t size;
    uint32_t start_sector;
    int used;
} fs_file_t;

typedef struct {
    fs_file_t files[FS_MAX_FILES];
    int num_files;
    int initialized;
} fs_t;

void fs_init(void);
int fs_open(const char* name);
int fs_read(int fd, void* buf, size_t count);
int fs_close(int fd);
int fs_list(void);

/* Userspace */
void shell_main(void);

/* Kernel entry */
void kernel_main(uint64_t multiboot_addr);

#endif
