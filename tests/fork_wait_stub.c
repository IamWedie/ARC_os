/*
 * Combined host-side hardware stub for the fork/wait suite.
 *
 * Unlike the older suites, this one links the REAL kernel/scheduler.c, so it
 * must stand in for every piece the scheduler borrows from the rest of the
 * kernel: physical frames, the TSS RSP0 write, the entry64 boot-stack symbol,
 * address-space hooks, and the fd lifecycle hooks. It deliberately does NOT
 * define processes[], current_process, num_processes or preempt_disabled --
 * scheduler.c owns those globals.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kernel.h"

static void fork_wait_stub_fs_reset(void);

/* ------------------------------------------------------------------ */
/* Physical frames: 64 MiB identity-mapped arena, first fit (pmm.c contract). */
/* ------------------------------------------------------------------ */

#define FORK_WAIT_STUB_PAGES 16384u /* 64 MiB */

static uint8_t  arena[FORK_WAIT_STUB_PAGES * 4096u]
    __attribute__((aligned(4096)));
static uint8_t  used[FORK_WAIT_STUB_PAGES];
static uint64_t used_pages;

void* pmm_alloc_pages(size_t count) {
    if (count == 0) count = 1;
    size_t run = 0;

    for (size_t i = 0; i < FORK_WAIT_STUB_PAGES; i++) {
        if (!used[i]) {
            run++;
            if (run == count) {
                size_t start = i + 1 - run;
                for (size_t j = start; j < start + count; j++) used[j] = 1;
                used_pages += count;
                return (void*)(arena + start * 4096u);
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

void pmm_free_pages(void* addr, size_t count) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)arena;
    if (a < base || a >= base + sizeof(arena)) return;
    if ((a - base) % 4096u) return;

    size_t start = (size_t)((a - base) / 4096u);
    for (size_t i = 0; i < count && start + i < FORK_WAIT_STUB_PAGES; i++) {
        if (used[start + i]) {
            used[start + i] = 0;
            used_pages--;
        }
    }
}

uint64_t pmm_total_mem(void) {
    return (uint64_t)FORK_WAIT_STUB_PAGES * 4096u;
}

uint64_t pmm_free_mem(void) {
    return (uint64_t)(FORK_WAIT_STUB_PAGES - used_pages) * 4096u;
}

/* Test-only introspection / isolation. */
uint64_t fork_wait_stub_used_pages(void) { return used_pages; }
void fork_wait_stub_reset(void) {
    for (size_t i = 0; i < FORK_WAIT_STUB_PAGES; i++) used[i] = 0;
    used_pages = 0;
    fork_wait_stub_fs_reset();
}

/* ------------------------------------------------------------------ */
/* Hardware / service hooks used by scheduler.c                        */
/* ------------------------------------------------------------------ */

uint64_t fork_wait_stub_tss_rsp0;
int fork_wait_stub_cr3_pid = -1;

void set_tss_rsp0(uint64_t rsp) { fork_wait_stub_tss_rsp0 = rsp; }

/* entry64.S normally provides this linker symbol for process 0. */
uint8_t __stack_top[64] __attribute__((aligned(16)));

void vmm_switch_address_space(int pid) { fork_wait_stub_cr3_pid = pid; }

/* ------------------------------------------------------------------ */
/* Address-space hooks used by real kernel/address_space.c              */
/* ------------------------------------------------------------------ */

/* Fake master root: only the shared kernel slot 0 is populated. The cloned
 * roots copy that slot verbatim; no host code ever loads it into CR3. */
static uint64_t fork_wait_stub_master[512] __attribute__((aligned(4096)));

uint64_t vmm_master_root(void) {
    return (uint64_t)(uintptr_t)fork_wait_stub_master;
}

void vmm_invalidate_page(uint64_t vaddr) { (void)vaddr; }

/* ------------------------------------------------------------------ */
/* Fake second embedded blob (exec.c's boot registry links against it).  */
/* ------------------------------------------------------------------ */

__asm__(".globl _binary_user_hello_bin_start\n"
        "_binary_user_hello_bin_start:\n"
        ".byte 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x0a\n"
        ".globl _binary_user_hello_bin_end\n"
        "_binary_user_hello_bin_end:\n"
        ".globl _binary_user_hello_bin_size\n"
        ".set _binary_user_hello_bin_size, 6\n");

/* ------------------------------------------------------------------ */
/* Console hooks used by real kernel/fd.c and kernel/syscall.c          */
/* ------------------------------------------------------------------ */

static char stub_console_out[256];
static size_t stub_console_out_len;
static const char stub_console_in[] = "a\n";
static size_t stub_console_in_pos;

void terminal_write(const char* buf, size_t count) {
    if (!buf) return;
    for (size_t i = 0; i < count && stub_console_out_len < sizeof(stub_console_out) - 1;
         i++) {
        stub_console_out[stub_console_out_len++] = buf[i];
    }
    stub_console_out[stub_console_out_len] = '\0';
}

void terminal_putstring(const char* s) {
    if (s) terminal_write(s, strlen(s));
}

void terminal_clear(void) {
    stub_console_out_len = 0;
    stub_console_out[0] = '\0';
}

int kbd_getchar(void) {
    if (stub_console_in[stub_console_in_pos] == '\0') return -1;
    return (unsigned char)stub_console_in[stub_console_in_pos++];
}

/* ------------------------------------------------------------------ */
/* Filesystem slot hooks used by real kernel/fd.c and kernel/syscall.c  */
/* ------------------------------------------------------------------ */

#define FORK_WAIT_STUB_SLOT_DATA 4096

typedef struct {
    int used;
    int deleted;
    int refs;
    uint32_t size;
    char name[FS_MAX_FILENAME];
    char data[FORK_WAIT_STUB_SLOT_DATA];
} stub_slot_t;

static stub_slot_t stub_slots[FS_MAX_FILES];
static unsigned stub_add_ref_calls;
static unsigned stub_release_calls;

static void fork_wait_stub_fs_reset(void) {
    memset(stub_slots, 0, sizeof(stub_slots));
    stub_add_ref_calls = 0;
    stub_release_calls = 0;
    stub_console_out_len = 0;
    stub_console_out[0] = '\0';
    stub_console_in_pos = 0;
}

unsigned fork_wait_stub_add_ref_calls(void) { return stub_add_ref_calls; }
unsigned fork_wait_stub_release_calls(void) { return stub_release_calls; }

int fork_wait_stub_total_refs(void) {
    int total = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (stub_slots[i].used) total += stub_slots[i].refs;
    }
    return total;
}

int fork_wait_stub_slot_for(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!stub_slots[i].used || stub_slots[i].deleted) continue;
        if (strcmp(stub_slots[i].name, name) == 0) return i;
    }
    return -1;
}

static int stub_slot_valid(int slot) {
    return slot >= 0 && slot < FS_MAX_FILES && stub_slots[slot].used;
}

static void stub_slot_set_name(stub_slot_t* slot, const char* name) {
    size_t i = 0;
    while (i < FS_MAX_FILENAME - 1 && name[i] != '\0') {
        slot->name[i] = name[i];
        i++;
    }
    slot->name[i] = '\0';
}

int fs_find_slot(const char* name) { return fork_wait_stub_slot_for(name); }

int fs_open_slot(const char* name, int create) {
    if (!name || name[0] == '\0') return -1;

    int existing = fork_wait_stub_slot_for(name);
    if (existing >= 0) return existing;
    if (!create) return -1;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (stub_slots[i].used) continue;
        memset(&stub_slots[i], 0, sizeof(stub_slots[i]));
        stub_slots[i].used = 1;
        stub_slot_set_name(&stub_slots[i], name);
        return i;
    }
    return -1;
}

int fs_slot_read(int slot, uint32_t off, void* buf, size_t count) {
    if (!stub_slot_valid(slot) || (!buf && count > 0)) return -1;
    if (count == 0 || off >= stub_slots[slot].size) return 0;

    size_t avail = stub_slots[slot].size - off;
    size_t n = count < avail ? count : avail;
    memcpy(buf, stub_slots[slot].data + off, n);
    return (int)n;
}

int fs_slot_write(int slot, uint32_t off, const void* buf, size_t count) {
    if (!stub_slot_valid(slot) || (!buf && count > 0)) return -1;
    if (count == 0) return 0;
    if (off > stub_slots[slot].size) return -1;
    if (off >= FORK_WAIT_STUB_SLOT_DATA) return 0;

    size_t room = FORK_WAIT_STUB_SLOT_DATA - off;
    size_t n = count < room ? count : room;
    if (n > 0) memcpy(stub_slots[slot].data + off, buf, n);
    if (off + n > stub_slots[slot].size) stub_slots[slot].size = (uint32_t)(off + n);
    return (int)n;
}

int fs_slot_size(int slot) {
    if (!stub_slot_valid(slot)) return -1;
    return (int)stub_slots[slot].size;
}

int fs_slot_truncate(int slot) {
    if (!stub_slot_valid(slot)) return -1;
    stub_slots[slot].size = 0;
    return 0;
}

int fs_slot_add_ref(int slot) {
    if (!stub_slot_valid(slot)) return -1;
    stub_slots[slot].refs++;
    stub_add_ref_calls++;
    return 0;
}

void fs_slot_release(int slot) {
    if (!stub_slot_valid(slot)) return;
    stub_release_calls++;
    if (stub_slots[slot].refs > 0) stub_slots[slot].refs--;
    if (stub_slots[slot].refs == 0 && stub_slots[slot].deleted) {
        memset(&stub_slots[slot], 0, sizeof(stub_slots[slot]));
    }
}

int fs_list(void) { return 0; }
int fs_delete(const char* name) { (void)name; return -1; }
int fs_rename(const char* oldname, const char* newname) {
    (void)oldname;
    (void)newname;
    return -1;
}
