/*
 * ARC OS Kernel Header
 * Updated with keyboard and process management
 * Slab allocator integrated
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

/* Console */
void terminal_initialize(void);
void terminal_putchar(char c, uint8_t color);
void terminal_putstring(const char* str);
void terminal_write(const char* buf, size_t count);
void terminal_setcolor(uint8_t color);
void terminal_clear(void);
void terminal_scroll(void);
void terminal_print_int(uint64_t n);
void terminal_print_hex(uint64_t n);

/* Physical memory manager (pmm.c) */
struct pmm_region {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t pad;
};

void pmm_init(uint64_t multiboot_addr);
void* pmm_alloc_pages(size_t count);
void pmm_free_pages(void* addr, size_t count);
uint64_t pmm_total_mem(void);
uint64_t pmm_free_mem(void);
uint32_t pmm_region_count_get(void);
struct pmm_region pmm_region_get(uint32_t i);

/* Virtual memory manager (vmm.c) */
void vmm_init(void);
void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags);
void vmm_unmap_page(uint64_t vaddr);
void vmm_create_user_mapping(uint64_t vaddr, uint64_t paddr, size_t pages);

/*
 * Page-table walk core (pagetable.c).
 *
 * Pure logic over identity-accessible memory: no CR3 load, no INVLPG, and no
 * implicit PMM use, so host tests can link it directly. ops->alloc must
 * return one zeroed 4 KiB page (or NULL); ops->free releases such a page.
 */
#define PT_PG_PRESENT (1ULL << 0)
#define PT_PG_RW      (1ULL << 1)
#define PT_PG_USER    (1ULL << 2)
#define PT_PG_PSE     (1ULL << 7)

/*
 * PML4 slot at which the kernel half begins (x86-64 canonical split at bit
 * 47): pt_count_user_pages() counts present 4 KiB leaves in slots below it.
 */
#define PT_USER_SLOT_FIRST 256

typedef struct {
    void* (*alloc)(void);
    void (*free)(void* page);
} pt_ops_t;

int pt_map(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags,
           const pt_ops_t* ops);
uint64_t* pt_lookup(uint64_t* root, uint64_t vaddr);
int pt_unmap(uint64_t* root, uint64_t vaddr);
int pt_count_user_pages(const uint64_t* root);

/* Heap (memory.c) */
void* kmalloc(size_t size);
void kfree(void* ptr);
int mem_selftest(void);
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
void* memmove(void* dest, const void* src, size_t n);
int strcmp(const char* a, const char* b);

/* Slab allocator (slab.c) */
struct kmem_cache;
void kmem_init_early(void);
void kmem_init_smp(void);
struct kmem_cache* kmem_cache_create(const char* name, uint64_t obj_size, uint64_t alignment);
void kmem_cache_destroy(struct kmem_cache* cache);
void* kmem_cache_alloc(struct kmem_cache* cache);
void kmem_cache_free(struct kmem_cache* cache, void* ptr);

/* IDT */
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

/* GDT / TSS */
#define GDTSEL_KC 0x08    /* kernel code, ring 0 */
#define GDTSEL_KD 0x10    /* kernel data, ring 0 */
#define GDTSEL_UD 0x18    /* user data, ring 3 -> 0x1B with RPL 3 */
#define GDTSEL_UC 0x20    /* user code, ring 3 -> 0x23 with RPL 3 */
#define GDTSEL_TSS 0x28   /* 64-bit TSS */
void gdt_init(void);
void set_tss_rsp0(uint64_t rsp);

/* Interrupts */
void interrupts_init(void);
void outb(uint16_t port, uint8_t val);
uint8_t inb(uint16_t port);
void irq0_stub(void);
void irq1_stub(void);
void serial_irq_stub(void);
uint64_t irq0_handler(uint64_t rsp);
int kbd_getchar(void);

/* CPU exceptions (vectors 0..31) */
void exception_gate_init(void);
void exception_handler(void* frame);

/* PIT timer */
void pit_init(void);
void pit_init_freq(uint32_t hz);

/* Scheduler */
#define MAX_PROCESSES 16
#define SCHED_MLFQ_LEVELS 4
#define SCHED_QUANTUM_TICKS 10    /* timer ticks before demotion */
#define SCHED_BOOST_TICKS 100     /* ticks between priority boosts */

typedef enum {
    PROC_FREE = 0,   /* slot unused */
    PROC_READY,      /* queued, waiting for CPU */
    PROC_RUNNING,    /* currently on the CPU */
    PROC_IDLE,       /* process 0 parked; runs only when no real work is ready */
    PROC_BLOCKED,    /* waiting for an event */
    PROC_ZOMBIE      /* exited, waiting to be reaped */
} proc_state_t;

/* A contiguous physical frame run owned by a process and freed on reap. */
#define PROCESS_MAX_OWNED_REGIONS 4

typedef struct {
    uint64_t base;   /* page-aligned physical address; 0 = empty slot */
    uint64_t pages;  /* number of 4 KiB frames in the run */
} proc_region_t;

/* Per-process file descriptors and shared open file descriptions (fd.c). */
#define FD_MAX 16
#define OFD_MAX 32

#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_ACCMODE 0x0003
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

typedef enum {
    FD_ENTRY_FREE = 0,
    FD_ENTRY_CONSOLE_IN,
    FD_ENTRY_CONSOLE_OUT,
    FD_ENTRY_CONSOLE_ERR,
    FD_ENTRY_FILE
} fd_entry_kind_t;

/*
 * An open file description. Fds in one or more processes point at an entry
 * in the fixed g_ofds[] pool; refs counts the fds sharing it.
 */
typedef struct {
    int used;
    int file_slot;
    uint32_t offset;
    uint32_t flags;
    unsigned refs;
} open_file_t;

typedef struct {
    fd_entry_kind_t kind;
    int ofd;          /* index into the global OFD table, or -1 */
    /* Reserved for future per-fd flags (e.g. close-on-exec). Always 0 today;
     * open flags live on open_file_t.flags, never here. */
    uint32_t flags;
} fd_entry_t;

typedef struct {
    uint64_t rsp;            /* saved kernel stack pointer (trap frame) */
    uint64_t rip;            /* entry point */
    uint64_t kstack_sp;      /* top of this process's kernel stack (TSS RSP0 on ring-3->0) */
    uint64_t stack_base;     /* base of the kernel stack (freed on reap) */
    uint64_t stack_size;     /* kernel stack size in bytes */
    uint64_t cpu_ticks;      /* timer ticks charged to this process */
    uint64_t switches_in;    /* times this process was switched in */
    proc_region_t owned[PROCESS_MAX_OWNED_REGIONS]; /* user frame runs */
    int user_mode;           /* nonzero if the entry frame uses user segments */
    int pid;
    int state;               /* proc_state_t */
    int level;               /* MLFQ level, 0 = highest priority */
    int exit_code;
    uint32_t slice_used;     /* ticks used in the current quantum */
    char name[32];
    fd_entry_t fds[FD_MAX];
} process_t;

typedef struct {
    uint64_t ticks;             /* timer preemption entries */
    uint64_t context_switches;  /* actual stack switches */
    uint64_t preemptions;       /* timer-driven scheduling passes */
    uint64_t yields;            /* voluntary scheduling passes */
    uint64_t demotions;         /* quantum expiries that lowered a level */
    uint64_t boosts;            /* priority-boost events */
    uint64_t idle_ticks;        /* preemptions that kept the idle process */
    uint32_t active;            /* non-FREE processes */
    uint32_t ready;             /* processes in ready queues */
    uint32_t blocked;
    uint32_t zombies;
    uint32_t idle;              /* process 0 parked while real work exists */
} scheduler_stats_t;

void scheduler_init(void);
void scheduler_yield(void);
uint64_t scheduler_switch(uint64_t rsp);
uint64_t scheduler_yield_handler(uint64_t rsp);
void scheduler_get_stats(scheduler_stats_t* out);
int scheduler_state(int pid);
int scheduler_block(int pid);
int scheduler_wake(int pid);
int scheduler_kill(int pid);
int scheduler_reap(int pid);
/*
 * Reap every zombie except the current process. Intended for the idle loop
 * (PID 0); returns the number reaped, or -1 if called from a non-idle process.
 * Preemption is disabled for the whole scan.
 */
int scheduler_reap_zombies(void);
void scheduler_boost(void);
void scheduler_block_current(void);
void scheduler_exit_current(int code) __attribute__((noreturn));
int scheduler_check_invariants(void);
int process_create(void (*entry)(void), const char* name);
int process_create_user(uint64_t entry, uint64_t kstack_top, const char* name);

/*
 * Allocate `pages` physical frames and register them as owned by `pid`.
 * Returns the page-aligned address, or NULL on failure. On failure nothing
 * remains allocated and no slot is consumed. Owned frames are released once
 * by scheduler_reap(); the kernel stack keeps its existing ownership fields.
 */
void* process_alloc_owned_frames(int pid, size_t pages);

void preempt_disable(void);
void preempt_enable(void);

/* Scheduler state exported for diagnostics and host tests. */
extern process_t processes[MAX_PROCESSES];
extern volatile int current_process;
extern volatile int num_processes;
extern volatile int preempt_disabled;

/* Assembly entry for the software-yield interrupt (vector 0x81). */
extern void yield_stub(void);

/* File descriptor table (fd.c). File-backed paths use the fs slot API. */
void fd_init_process(process_t* p);
void fd_close_all(process_t* p);
int fd_clone_process(const process_t* parent, process_t* child);
int fd_open(int pid, const char* name, uint32_t flags);
int fd_dup(int pid, int oldfd);
int fd_close(int pid, int fd);
/* Non-blocking query used by the syscall boundary to decide whether a read
 * may sleep waiting for keyboard/serial input. No inline asm here: host tests
 * link fd.c directly. */
int fd_is_console_in(int pid, int fd);
int fd_read(int pid, int fd, void* buf, size_t count);
int fd_write(int pid, int fd, const void* buf, size_t count);
int fd_seek(int pid, int fd, int64_t offset, int whence);
int fd_size(int pid, int fd);
int fd_truncate(int pid, int fd);

/* Filesystem */
#define FS_SECTOR_SIZE 512
#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILESIZE 65536

typedef struct {
    char name[FS_MAX_FILENAME];
    uint32_t size;
    uint32_t start_block;   /* first 512-byte block of the extent */
    uint32_t blocks;        /* number of blocks in the extent */
    /* Legacy fd-shaped read/write position. Persisted on disk as part of the
     * version-2 directory record (on-disk layout compatibility); the fd layer
     * now keeps the live position on open_file_t.offset, so this field is
     * normalized to 0 on mount and write but must not be dropped. */
    uint32_t pos;
    uint16_t open_refs;     /* open file descriptions referencing this slot */
    uint8_t deleted;        /* unlinked name; extent freed at last release */
    int used;               /* file exists (including deleted-but-open) */
} fs_file_t;

void fs_init(void);
int fs_delete(const char* name);
int fs_list(void);
int fs_rename(const char* oldname, const char* newname);

/*
 * Slot-level filesystem access used by the descriptor layer. Slots are
 * fs_file_t indices, not descriptor numbers.
 */
int fs_open_slot(const char* name, int create);
/* Resolve a visible (non-deleted) name to a slot; -1 when absent. */
int fs_find_slot(const char* name);
int fs_slot_read(int slot, uint32_t off, void* buf, size_t count);
int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count);
int fs_slot_size(int slot);
int fs_slot_truncate(int slot);
int fs_slot_add_ref(int slot);
void fs_slot_release(int slot);

/* Serial (COM1) */
void serial_putchar(char c);
void serial_write(const char* s, uint32_t len);

/*
 * Non-blocking COM1 receive. Returns a byte (CR translated to LF) or -1 when
 * no data is waiting. Polled from kbd_getchar so the shell works headlessly
 * with `qemu -serial stdio`, not just through the PS/2 keyboard.
 */
int serial_poll(void);
void serial_irq_handler(void);
int serial_init(void);

/* ACPI / APIC */
void acpi_init(void);
uint32_t acpi_lapic_base(void);
uint32_t acpi_ioapic_base(void);
int acpi_cpu_total(void);
int acpi_iso_get(int irq, int* gsi, int* pol, int* trig);
void lapic_init(void);
void lapic_eoi(void);
void lapic_mask_extint(void);
void lapic_timer_start(void);
uint64_t apic_timer_handler(uint64_t rsp);
void apic_timer_stub(void);
extern volatile int pit_tick_count;

/* IOAPIC */
void ioapic_init(void);
int ioapic_active(void);

/* ATA PIO */
int ata_init(void);
int ata_present(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf);
void ata_irq_handler(void);
void ata_irq_stub(void);

/* Userspace */
long syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3);
void syscall_stub(void);
int user_process_create_from_blob(void);

/* Embedded ring-3 shell blob (symbols generated by objcopy). */
extern const uint8_t _binary_user_shell_bin_start[];
extern const uint8_t _binary_user_shell_bin_end[];
extern const uint8_t _binary_user_shell_bin_size[];

/* User virtual memory */
#define USER_CODE_VA 0x8000000000ULL
#define USER_STACK_VA 0x9000000000ULL
#define USER_STACK_PAGES 32
#define USER_STACK_TOP (USER_STACK_VA + (USER_STACK_PAGES * 4096ULL))
#define USER_FRAME_PAGES 32

/* Syscall numbers (user ABI) */
#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_EXIT  3
#define SYS_OPEN  4
#define SYS_CLOSE 5
#define SYS_LS    6
#define SYS_DEL   7
#define SYS_SEEK  8
#define SYS_SIZE  9
#define SYS_TRUNC 10
#define SYS_RENAME 11
#define SYS_CLEAR 12
#define SYS_DUP   13

/* Kernel entry */
void kernel_main(uint64_t multiboot_addr);

/* Symbols provided by linker/kernel64.ld. */
extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t __stack_top[];
extern uint8_t __userblob_end[];

#endif