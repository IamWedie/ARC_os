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
struct bootinfo;

struct pmm_region {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t pad;
};

void pmm_init(const struct bootinfo* bi);
void* pmm_alloc_pages(size_t count);
void* pmm_alloc_pages_below4g(size_t count);
void pmm_free_pages(void* addr, size_t count);
void pmm_reserve_range(uint64_t base, uint64_t len);
uint64_t pmm_total_mem(void);
uint64_t pmm_free_mem(void);
uint32_t pmm_region_count_get(void);
struct pmm_region pmm_region_get(uint32_t i);

/* Virtual memory manager (vmm.c) */
void vmm_init(void);
void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint8_t flags);
void vmm_unmap_page(uint64_t vaddr);
uint64_t vmm_master_root(void);
void vmm_invalidate_page(uint64_t vaddr);
void vmm_switch_address_space(int pid);

/* Per-process address spaces (address_space.c) */
int  as_create(int pid);
int  as_map_user(int pid, uint64_t vaddr, uint64_t paddr,
                 size_t pages, uint8_t flags);
/*
 * Deep-copy the private user leaves of src_pid into a fresh address space for
 * dst_pid. Returns 0 on success, -1 on bad pids or resource failure; on
 * failure the child root is still valid and must be reaped by the caller.
 */
int  as_clone(int src_pid, int dst_pid);
void as_destroy(int pid);
uint64_t as_root(int pid);
/*
 * Atomically install a fresh user half for pid: code_pages at USER_CODE_VA and
 * stack_pages at USER_STACK_VA, with the master kernel slot shared. Returns 0
 * on success, -1 on bad arguments or resource failure; on failure the process
 * keeps its old root and owned[] frames are never touched.
 */
int  as_replace_user(int pid, uint64_t code_pa, size_t code_pages,
                     uint64_t stack_pa, size_t stack_pages);

/*
 * Exec loader (exec.c). exec_build_user_stack returns the user VA of argc;
 * exec_load_image returns 0 on success or -1 with no mutation on failure.
 */
#define EXEC_MAX_ARGS 8
#define EXEC_MAX_ARG_LEN 128
uint64_t exec_build_user_stack(uint8_t* stack_base, size_t stack_bytes,
                               char* const argv[], int argc);
int exec_load_image(int pid, const uint8_t* image, size_t size,
                    char* const argv[], int argc, uint64_t* user_rsp_out);
/* Replace the current user image from a checked user-supplied path/argv.
 * 0 on success (the trap frame now enters the new image), -1 on failure with
 * no change to the process image, address space, fds or trap frame. */
long sys_exec(const char* path, char* const argv[]);
/* Install the embedded user programs into the RAM FS at boot. */
void exec_install_programs(void);

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
#define PT_PG_PWT     (1ULL << 3)
#define PT_PG_PCD     (1ULL << 4)
#define PT_PG_PSE     (1ULL << 7)

/*
 * First private (user) PML4 slot. Only slot 0 is shared kernel: the master
 * root's identity map lives there. Both user VAs (0x8000000000 and
 * 0x9000000000) are PML4 slot 1; slot 256 would start at 2^47.
 * pt_count_user_pages() counts present 4 KiB leaves in slots at or above it.
 */
#define PT_USER_SLOT_FIRST 1

typedef struct {
    void* (*alloc)(void);
    void (*free)(void* page);
} pt_ops_t;

int pt_map(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags,
           const pt_ops_t* ops);
uint64_t* pt_lookup(uint64_t* root, uint64_t vaddr);
int pt_unmap(uint64_t* root, uint64_t vaddr);
void pt_clone_shared(uint64_t* src, uint64_t* dst);
void pt_free_user_tables(uint64_t* root, const pt_ops_t* ops);
int pt_count_user_pages(const uint64_t* root);
/* Visit every present 4 KiB leaf in the private half of root, in VA order. */
void pt_foreach_user_leaf(const uint64_t* root,
                          void (*cb)(uint64_t va, uint64_t pa, uint64_t flags,
                                     void* ctx),
                          void* ctx);

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
void outl(uint16_t port, uint32_t val);
uint32_t inl(uint16_t port);
void irq0_stub(void);
void irq1_stub(void);
void irq12_stub(void);
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
    uint64_t pml4_phys;      /* physical address of the private root; 0 = master */
    uint64_t cpu_ticks;      /* timer ticks charged to this process */
    uint64_t switches_in;    /* times this process was switched in */
    proc_region_t owned[PROCESS_MAX_OWNED_REGIONS]; /* user frame runs */
    int user_mode;           /* nonzero if the entry frame uses user segments */
    int pid;
    int ppid;                /* parent pid; 0 for the kernel/idle or orphans */
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
 * Reap every unparented zombie (ppid == 0) except the current process. Zombies
 * with a live parent are left for that parent's wait(). Intended for the idle
 * loop (PID 0); returns the number reaped, or -1 if called from a non-idle
 * process. Preemption is disabled for the whole scan.
 */
int scheduler_reap_zombies(void);
/*
 * Collect a zombie child of parent_pid: returns its pid after reaping it and
 * copying exit_code to status_out (when non-NULL), 0 if live children exist
 * but none is collectable yet, or -1 (ECHILD) if there are no children.
 */
int scheduler_wait_child(int parent_pid, int* status_out);
void scheduler_boost(void);
void scheduler_block_current(void);
/*
 * Exit bookkeeping used by scheduler_exit_current: adopt orphaned children,
 * publish exit_code, mark the zombie and wake a blocked parent. Exposed so
 * host tests can exercise the policy without the noreturn yield loop.
 */
void scheduler_exit_process(int pid, int code);
void scheduler_exit_current(int code) __attribute__((noreturn));
int scheduler_check_invariants(void);
int process_create(void (*entry)(void), const char* name);
int process_create_user(uint64_t entry, uint64_t kstack_top, const char* name);
/*
 * Trap frame captured by syscall_stub: pushq $0 (return slot) then PUSHREGS
 * (15 GPRs in r15..rax order), followed by the CPU's iretq words. 21 words /
 * 168 bytes total. syscall_frame_rsp points at word 0 (saved r15).
 */
typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, ret_slot;
    uint64_t rip, cs, rflags, user_rsp, ss;
} syscall_frame_t;

extern volatile uint64_t syscall_frame_rsp;

/*
 * Fork a user process from its captured int 0x80 trap frame. The child gets a
 * fresh kernel stack whose saved context uses the timer-stub layout (15 GPRs +
 * iretq, no return slot) and whose saved rax is 0. Returns the child pid, or
 * -1 for a bad/kernel-mode parent or resource exhaustion. The caller must
 * still clone the address space and fd table (see as_clone/fd_clone_process).
 */
int process_fork(int parent_pid, const syscall_frame_t* frame);

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
int fd_fsync(int pid, int fd);

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
struct arc_dirent;
int fs_readdir(const char* path, uint32_t index, struct arc_dirent* out);
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

/* M4 VFS / persistence */
struct blockdev;
struct arc_stat;
struct arc_dirent;
#define VFS_NAME_MAX 64
#define VFS_MAX_MOUNTS 4

struct arc_stat {
    uint32_t size;
    uint32_t is_dir;
    uint32_t attrs;
    uint32_t reserved;
};

struct arc_dirent {
    char name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t is_dir;
};

struct vfs_node;
struct vfs_ops {
    int (*open)(void* ctx, const char* path, int create, int* out_slot);
    int (*read)(void* ctx, int slot, uint32_t off, void* buf, size_t count);
    int (*write)(void* ctx, int slot, uint32_t off, const void* buf, size_t count);
    int (*size)(void* ctx, int slot);
    int (*truncate)(void* ctx, int slot);
    int (*close_slot)(void* ctx, int slot);
    int (*add_ref)(void* ctx, int slot);
    void (*release)(void* ctx, int slot);
    int (*stat)(void* ctx, const char* path, struct arc_stat* st);
    int (*readdir)(void* ctx, const char* path, uint32_t index, struct arc_dirent* out);
    int (*mkdir)(void* ctx, const char* path);
    int (*unlink)(void* ctx, const char* path);
    int (*rename)(void* ctx, const char* oldname, const char* newname);
    int (*sync)(void* ctx);
};

int vfs_mount(const char* path, const struct vfs_ops* ops, void* ctx, struct blockdev* dev);
int vfs_stat(const char* path, struct arc_stat* st);
int vfs_readdir(const char* path, uint32_t index, struct arc_dirent* out);
int vfs_mkdir(const char* path);
int vfs_unlink(const char* path);
int vfs_mount_device(const char* devname, const char* fstype, const char* target);
int vfs_sync(const char* mount);
int vfs_sync_all(void);

/* Partition */
struct part_info {
    uint32_t lba_start;
    uint32_t sector_count;
    uint8_t type;
    uint8_t valid;
    uint8_t gpt_type; /* 0=MBR/none, 1=Basic Data, 2=ESP, 3=other GPT */
};
int part_probe(struct blockdev* dev, struct part_info* out, int max);
int storage_part_probe_fat32(const struct part_info *p, int qemu_fallback_pass, int dev_is_qemu);
int storage_root_can_select(int qualifies_arcroot, int is_esp, int qemu_fallback_pass, int dev_is_qemu);
int storage_is_esp(const struct part_info *p);
void ata_ident_string(char *dst, int dst_len, const uint16_t *src_words, int word_count);
void ata_identify_model(char *dst, int dst_len, const uint16_t words[256]);
int storage_model_is_qemu(const char *model);

/* FAT32 */
int fat32_mount(struct blockdev* dev, uint32_t lba_start, uint32_t sector_count);
int fat32_unmount(void);
int fat32_is_mounted(void);
int fat32_qualifies_arcroot(void);
int fat32_open_slot(const char* name, int create);
int fat32_slot_read(int slot, uint32_t off, void* buf, size_t count);
int fat32_slot_write(int slot, uint32_t off, const void* buf, size_t count);
int fat32_slot_size(int slot);
int fat32_slot_truncate(int slot);
int fat32_slot_add_ref(int slot);
void fat32_slot_release(int slot);
int fat32_delete(const char* name);
int fat32_rename(const char* oldname, const char* newname);
int fat32_list(void);
int fat32_stat(const char* path, struct arc_stat* st);
int fat32_readdir(const char* path, uint32_t index, struct arc_dirent* out);
int fat32_mkdir(const char* path);

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

/* PCI */
#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, progif, revision;
    uint8_t header_type;
    uint8_t irq_line, irq_pin;
    uint32_t bar[6];
};

void pci_init(void);
int pci_device_count(void);
const struct pci_device* pci_device_get(int i);
const struct pci_device* pci_find_class(uint8_t class_code, uint8_t subclass, int idx);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);

/* AHCI (M4.4) */
#define AHCI_CAP2_BOH           (1u << 0)
#define AHCI_BOHC_BOS           (1u << 0)
#define AHCI_BOHC_OOS           (1u << 1)
#define AHCI_BOHC_BB            (1u << 4)

#define AHCI_BOHC_UNSUPPORTED   0
#define AHCI_BOHC_ALREADY_OWNED 1
#define AHCI_BOHC_SUCCESS       2
#define AHCI_BOHC_BB_TIMEOUT    3

typedef struct {
    uint32_t cap2;
    uint32_t bohc;
} ahci_bohc_state_t;

void ahci_init(void);
int ahci_present(void);
uint32_t ahci_bar5(void);

#define AHCI_SIG_ATA    0x00000101u
#define AHCI_SIG_ATAPI  0xEB140101u
#define AHCI_SIG_SEMB   0xC33C0101u
#define AHCI_SIG_PM     0x96690101u

#define AHCI_PORT_CMD_ST  (1u << 0)
#define AHCI_PORT_CMD_FRE (1u << 4)
#define AHCI_PORT_CMD_FR  (1u << 14)
#define AHCI_PORT_CMD_CR  (1u << 15)

#define AHCI_PORT_TFD_BSY (1u << 7)
#define AHCI_PORT_TFD_DRQ (1u << 3)
#define AHCI_PORT_TFD_ERR (1u << 0)
#define AHCI_PORT_IS_TFES (1u << 30)

typedef struct {
    uint32_t cmd;
    uint32_t tfd;
    uint32_t serr;
    uint32_t is;
    uint32_t ssts;
    uint32_t sig;
    uint32_t ci;
} ahci_fake_port_t;

typedef struct {
    int port;
    uint32_t sig;
    uint64_t clb;
    uint64_t fb;
    uint64_t ctba;
    int started;
    int busy;
    int failed;
    int identify_ok;
    char model[41];
    char serial[21];
    char firmware[9];
    uint64_t sectors;
    int lba48;
    uint32_t sector_size;
    int flush_supported;
    int flush_ext_supported;
    int rw_dma_ext_supported;
    int writable;
    int bd_registered;
    int bd_failed_logged;
    char bd_name[8];
} ahci_port_state_t;

/* Host-testable AHCI helpers */
int ahci_bar_is_mmio32(uint32_t bar);
uint32_t ahci_bar_base(uint32_t bar);
uint32_t ahci_cap_np(uint32_t cap);
int ahci_cap_s64a(uint32_t cap);
uint32_t ahci_pi_popcount(uint32_t pi);
int ahci_bohc_run(ahci_bohc_state_t *r,
                  int (*wait_bos_clear)(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms),
                  int (*wait_bb_clear)(ahci_bohc_state_t *r, uint32_t timeout_ms, uint32_t *elapsed_ms),
                  uint32_t *elapsed_ms);
uint32_t ahci_ssts_det(uint32_t ssts);
uint32_t ahci_ssts_ipm(uint32_t ssts);
int ahci_sig_is_sata(uint32_t sig);
const char* ahci_sig_name(uint32_t sig);
int ahci_port_layout_ok(uint64_t page_phys);
int ahci_port_stop_sim(ahci_fake_port_t *p,
                       int (*wait_clear)(ahci_fake_port_t *p, uint32_t mask, uint32_t timeout_ms, uint32_t *elapsed_ms),
                       uint32_t *elapsed_ms);
int ahci_port_start_sim(ahci_fake_port_t *p,
                        int (*wait_tfd_ready)(ahci_fake_port_t *p, uint32_t timeout_ms, uint32_t *elapsed_ms),
                        uint32_t *elapsed_ms);

#define AHCI_FIS_TYPE_REG_H2D 0x27u
#define ATA_CMD_IDENTIFY      0xECu

typedef struct {
    uint8_t fis[20];
} ahci_h2d_fis_t;

typedef struct {
    uint32_t dw0;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} ahci_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc_i;
} ahci_prdt_entry_t;

typedef struct {
    char model[41];
    char serial[21];
    char firmware[9];
    uint64_t sectors;
    int lba48;
    uint32_t sector_size;
    uint32_t phys_sector_mult;
    int flush_supported;
    int flush_ext_supported;
    int rw_dma_ext_supported;
    int lba48_assumed_enabled;
    int supported;
} ahci_identify_result_t;

void ahci_build_h2d_identify(uint8_t fis[20]);
void ahci_build_cmd_header(ahci_cmd_header_t *h, uint32_t ctba, uint32_t ctbau);
void ahci_build_prdt(ahci_prdt_entry_t *p, uint64_t dba, uint32_t byte_count);
int ahci_identify_parse(const uint16_t words[256], ahci_identify_result_t *out);
int ahci_identify_poll_sim(ahci_fake_port_t *p,
                           int (*step)(ahci_fake_port_t *p, uint32_t iter),
                           uint32_t max_iters,
                           uint32_t *prdbc_out);
int ahci_cmd_poll_sim(ahci_fake_port_t *p,
                      int (*step)(ahci_fake_port_t *p, uint32_t iter),
                      uint32_t max_iters,
                      uint32_t expected_prdbc,
                      uint32_t *prdbc_out);
int ahci_walk_virt_to_phys(const uint64_t *pml4, uint64_t va, uint64_t *pa_out);
int ahci_port_can_register(int identify_ok, int started, uint32_t sector_size, int rw_dma_ext_supported, int failed);
int ahci_write_policy_writable(const char *model, int is_root, int has_flush_ext, int arc_ahci_write_real);
int ahci_split_rw(uint64_t total_sectors, uint64_t lba, uint32_t count,
                  int (*do_chunk)(void *ctx, uint64_t lba, uint32_t count, uint32_t done), void *ctx,
                  uint32_t *done_out);
void ahci_apply_write_policy(const char* root_name);
int ahci_dev_model_is_qemu(const char* name);

#define AHCI_MAX_SECTORS 128u

#define AHCI_OK              0
#define AHCI_ERR_BADARG     -1
#define AHCI_ERR_RANGE      -2
#define AHCI_ERR_UNSUPPORTED -3
#define AHCI_ERR_BUSY       -4
#define AHCI_ERR_TIMEOUT    -5
#define AHCI_ERR_DEV        -6

#define ATA_CMD_READ_DMA_EXT    0x25u
#define ATA_CMD_WRITE_DMA_EXT   0x35u
#define ATA_CMD_FLUSH_CACHE_EXT 0xEAu

extern int ahci_writes_enabled;

void ahci_build_h2d_rw(uint8_t fis[20], uint8_t cmd, uint64_t lba, uint16_t count);
void ahci_build_h2d_flush(uint8_t fis[20]);
void ahci_build_cmd_header_ex(ahci_cmd_header_t *h, uint16_t prdtl, int write, uint32_t ctba, uint32_t ctbau);
int ahci_prdt_build(ahci_prdt_entry_t *prdt, uint32_t max_entries, uint64_t phys_addr, uint32_t bytes);
typedef uint64_t (*ahci_phys_lookup_fn)(void *ctx, uint64_t va, int *ok);
int ahci_prdt_build_from_va(ahci_prdt_entry_t *prdt, uint32_t max_entries,
                            ahci_phys_lookup_fn lookup, void *ctx,
                            uint64_t va, uint32_t bytes);
int ahci_validate_rw_args(const ahci_port_state_t *ps, uint64_t lba, uint32_t count, const void *buf);
int ahci_validate_flush_args(const ahci_port_state_t *ps);
int ahci_error_from_status(uint32_t is, uint32_t tfd);

int ahci_read(uint32_t port, uint64_t lba, uint32_t count, void* buf);
int ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void* buf);
int ahci_flush(uint32_t port);

/* Block device */
#define BLOCK_SECTOR_SIZE 512
#define BLOCKDEV_MAX 4

struct blockdev {
    const char* name;
    uint32_t sector_count;
    int (*read)(struct blockdev* dev, uint32_t lba, uint32_t count, void* buf);
    int (*write)(struct blockdev* dev, uint32_t lba, uint32_t count, const void* buf);
    int (*flush)(struct blockdev* dev);
    void* ctx;
    int read_only;
};

int blockdev_register(struct blockdev* dev);
struct blockdev* blockdev_get(int idx);
int blockdev_count(void);
struct blockdev* blockdev_find(const char* name);
int blockdev_read(struct blockdev* dev, uint32_t lba, uint32_t count, void* buf);
int blockdev_write(struct blockdev* dev, uint32_t lba, uint32_t count, const void* buf);
int blockdev_flush(struct blockdev* dev);

/* ATA PIO */
int ata_init(void);
int ata_present(void);
int ata_model_is_qemu(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf);
void ata_irq_handler(void);
void ata_irq_stub(void);

/* Userspace */
long syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3);
/* Fork/wait syscall bodies (kernel/fork.c). sys_fork consumes the frame
 * published by syscall_stub through syscall_frame_rsp. */
long sys_fork(void);
long sys_wait(int* status);
void syscall_stub(void);
int user_process_create_from_blob(void);

/* Embedded ring-3 blobs (symbols generated by objcopy). */
extern const uint8_t _binary_user_shell_bin_start[];
extern const uint8_t _binary_user_shell_bin_end[];
extern const uint8_t _binary_user_shell_bin_size[];
extern const uint8_t _binary_user_hello_bin_start[];
extern const uint8_t _binary_user_hello_bin_end[];
extern const uint8_t _binary_user_hello_bin_size[];

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
#define SYS_FORK  14
#define SYS_WAIT  15
#define SYS_EXEC  16
#define SYS_STAT    17
#define SYS_READDIR 18
#define SYS_MKDIR   19
#define SYS_UNLINK  20
#define SYS_MOUNT   21
#define SYS_SYNC    22
#define SYS_FSYNC   23

/* Kernel entry */
void kernel_main(uint64_t boot_info, uint64_t boot_magic);

/* Symbols provided by linker/kernel64.ld. */
extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t __stack_top[];
extern uint8_t __userblob_end[];

#endif