/*
 * ARC OS Kernel Header
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
int mem_selftest(void);
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int strlen(const char* s);
void* memmove(void* dest, const void* src, size_t n);
int strcmp(const char* a, const char* b);

/* Physical memory manager */
struct pmm_region {
    uint64_t base;
    uint64_t len;
    int type;
};
void pmm_init(uint64_t multiboot_addr);
void* pmm_alloc_pages(size_t count);
void pmm_free_pages(void* addr, size_t count);
uint64_t pmm_total_mem(void);
uint64_t pmm_free_mem(void);
uint32_t pmm_region_count_get(void);
struct pmm_region pmm_region_get(uint32_t i);

/* Virtual memory manager */
#define PG_PRESENT 1
#define PG_WRITABLE 2
#define PG_USER 4

void vmm_init(void);
void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags);
void vmm_unmap_page(uint64_t vaddr);
void vmm_create_user_mapping(uint64_t vaddr, uint64_t paddr, size_t pages);

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];
extern unsigned char __stack_top[];

/* GDT */
#define GDTSEL_KC 0x08
#define GDTSEL_KD 0x10
#define GDTSEL_UD 0x18
#define GDTSEL_UC 0x20
#define GDTSEL_TSS 0x28

void gdt_init(void);
void set_tss_rsp0(uint64_t rsp);

extern int tss_registered;

/* IDT */
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

/* Interrupts */
void interrupts_init(void);
void outb(uint16_t port, uint8_t val);
uint8_t inb(uint16_t port);
void irq0_stub(void);
void irq1_stub(void);
uint64_t irq0_handler(uint64_t rsp);
int kbd_getchar(void);

/* PIT timer */
void pit_init(void);
void pit_init_freq(uint32_t hz);

/* Scheduler */
#define MAX_PROCESSES 16

typedef struct {
    uint64_t rsp;            /* saved kernel stack pointer (trap frame) */
    uint64_t rip;            /* entry point */
    uint64_t kstack_sp;      /* top of this process's kernel stack (TSS RSP0 on ring-3->0) */
    int user_mode;           /* nonzero if the entry frame uses user segments */
    int pid;
    int running;
    int started;
    uint64_t stack_base;
    uint64_t stack_size;
    char name[32];
} process_t;

void scheduler_init(void);
void scheduler_yield(void);
uint64_t scheduler_switch(uint64_t rsp);
void process_create(void (*entry)(void), const char* name);
int process_create_user(uint64_t entry, uint64_t kstack_top, const char* name);
void preempt_disable(void);
void preempt_enable(void);

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
long syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3);
void syscall_stub(void);
int user_process_create_from_blob(void);

extern unsigned char _binary_user_shell_bin_start[];
extern unsigned char _binary_user_shell_bin_size[];

#define USER_CODE_VA 0x8000000000ULL
#define USER_STACK_VA 0x9000000000ULL
#define USER_STACK_PAGES 32
#define USER_STACK_TOP (USER_STACK_VA + (USER_STACK_PAGES * 4096ULL))
#define USER_FRAME_PAGES 32

/* Syscall numbers (user ABI) */
#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_EXIT  3

/* Kernel entry */
void kernel_main(uint64_t multiboot_addr);

#endif
