/*
 * Host-side stubs for scheduler tests.
 *
 * scheduler.c owns scheduling policy; these stubs stand in for the two
 * hardware-facing pieces it needs: the TSS RSP0 write and the entry64 stack
 * symbol used by process 0.  PMM allocation comes from tests/pmm_stub.c.
 */
#include <stdint.h>

uint64_t scheduler_stub_tss_rsp0;

void set_tss_rsp0(uint64_t rsp) {
    scheduler_stub_tss_rsp0 = rsp;
}

/* entry64.S normally provides this linker symbol. */
uint8_t __stack_top[64] __attribute__((aligned(16)));
