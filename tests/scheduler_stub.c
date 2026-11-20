/*
 * Host-side stubs for scheduler tests.
 *
 * scheduler.c owns scheduling policy; these stubs stand in for the two
 * hardware-facing pieces it needs: the TSS RSP0 write and the entry64 stack
 * symbol used by process 0.  PMM allocation comes from tests/pmm_stub.c.
 */
#include "kernel.h"

/* scheduler.c calls the fd lifecycle hooks; this suite does not link fd.c. */
void fd_init_process(process_t* p) { (void)p; }
void fd_close_all(process_t* p) { (void)p; }

uint64_t scheduler_stub_tss_rsp0;

void set_tss_rsp0(uint64_t rsp) {
    scheduler_stub_tss_rsp0 = rsp;
}

/* entry64.S normally provides this linker symbol. */
uint8_t __stack_top[64] __attribute__((aligned(16)));

/* Address-space hooks: record the requested CR3 pid and teardown calls. */
int scheduler_stub_cr3_pid = -1;
int as_destroy_calls;
void vmm_switch_address_space(int pid) { scheduler_stub_cr3_pid = pid; }
void as_destroy(int pid) { (void)pid; as_destroy_calls++; }
