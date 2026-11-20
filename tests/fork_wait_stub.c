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

#include "kernel.h"

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
}

/* ------------------------------------------------------------------ */
/* Hardware / service hooks used by scheduler.c                        */
/* ------------------------------------------------------------------ */

uint64_t fork_wait_stub_tss_rsp0;
int fork_wait_stub_cr3_pid = -1;
int fork_wait_stub_as_destroy_calls;

void set_tss_rsp0(uint64_t rsp) { fork_wait_stub_tss_rsp0 = rsp; }

/* entry64.S normally provides this linker symbol for process 0. */
uint8_t __stack_top[64] __attribute__((aligned(16)));

void vmm_switch_address_space(int pid) { fork_wait_stub_cr3_pid = pid; }

void as_destroy(int pid) {
    (void)pid;
    fork_wait_stub_as_destroy_calls++;
}

/* This suite does not link fd.c, so stand in for its two lifecycle hooks. */
void fd_init_process(process_t* p) {
    if (!p) return;
    for (int i = 0; i < FD_MAX; i++) {
        p->fds[i].kind = FD_ENTRY_FREE;
        p->fds[i].ofd = -1;
        p->fds[i].flags = 0;
    }
    p->fds[FD_STDIN].kind = FD_ENTRY_CONSOLE_IN;
    p->fds[FD_STDOUT].kind = FD_ENTRY_CONSOLE_OUT;
    p->fds[FD_STDERR].kind = FD_ENTRY_CONSOLE_ERR;
}

void fd_close_all(process_t* p) {
    (void)p;
}
