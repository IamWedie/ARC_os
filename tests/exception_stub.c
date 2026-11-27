/*
 * Host-side stub for the user-exception isolation suite.
 *
 * The test links the REAL kernel/scheduler.c, so this file provides only the
 * hardware/service hooks scheduler.c borrows: TSS RSP0, the entry64 idle
 * stack symbol, CR3 switching, address-space teardown, and fd lifecycle.
 * Address-space cleanup is counted instead of performed so the test can prove
 * a faulting process is reclaimed exactly once.
 */
#include <stddef.h>
#include <stdint.h>

#include "kernel.h"

extern uint64_t pmm_stub_used_pages(void);

uint64_t exception_stub_tss_rsp0;
int exception_stub_cr3_pid = -1;
int exception_stub_as_destroy_calls;

void set_tss_rsp0(uint64_t rsp) { exception_stub_tss_rsp0 = rsp; }

/* entry64.S normally provides this linker symbol for process 0. */
uint8_t __stack_top[64] __attribute__((aligned(16)));

void vmm_switch_address_space(int pid) { exception_stub_cr3_pid = pid; }

void as_destroy(int pid) {
    (void)pid;
    exception_stub_as_destroy_calls++;
}

void fd_init_process(process_t* p) { (void)p; }
void fd_close_all(process_t* p) { (void)p; }

uint64_t exception_stub_used_pages(void) { return pmm_stub_used_pages(); }

void exception_stub_reset(void) {
    exception_stub_tss_rsp0 = 0;
    exception_stub_cr3_pid = -1;
    exception_stub_as_destroy_calls = 0;
}
