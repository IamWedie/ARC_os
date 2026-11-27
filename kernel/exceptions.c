#include "kernel.h"

/*
 * CPU exception handling (utility vectors 0..31). Each IDT gate jumps to an
 * assembly stub (isr.S) that keeps the hardware error code (or pushes a
 * dummy), pushes the vector number, saves all GPRs and calls us with a
 * pointer to the shared exception_frame_t. Ring-0 faults enter the one
 * centralized fatal path and never return. Ring-3 faults terminate only the
 * faulting process; the stub returns on the scheduler's replacement stack, so
 * the faulting instruction is never resumed. Double faults always use the
 * kernel-fatal path regardless of the saved CS selector.
 */

extern uint64_t exc_stub_table[32];

void exception_gate_init(void) {
    for (int i = 0; i < 32; i++)
        idt_set_exception_gate((uint8_t)i, exc_stub_table[i]);
}

uint64_t exception_handler(void* fptr) {
    exception_frame_t* f = (exception_frame_t*)fptr;
    if (!f) kernel_panic("CPU exception with null frame");

    /*
     * Only the saved CS RPL decides normal exception origin. A fault inside a
     * syscall while a user process is scheduled has CS.RPL == 0 and must still
     * be fatal; exception_frame_is_fatal() also pins #DF to the fatal path.
     */
    if (exception_frame_is_fatal(f)) kernel_panic_exception(f);

    uint32_t v = (uint32_t)(f->vec & 0xFFULL);
    terminal_setcolor(COLOR_WHITE | (COLOR_BLUE << 4));
    terminal_putstring("\n[User fault] ");
    terminal_putstring(panic_exception_name(v));
    terminal_putstring(" vector=");
    terminal_print_int(v);
    terminal_putstring(" pid=");
    terminal_print_int((uint64_t)current_process);
    terminal_putstring(" error=");
    terminal_print_hex(f->err);
    if (v == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_putstring(" address=");
        terminal_print_hex(cr2);
    }
    terminal_putstring(" terminating process\n");

    uint64_t next_rsp = exception_user_fault_exit(f, (uint64_t)f);
    if (next_rsp == 0) {
        /* No safe replacement stack: halt rather than resume the bad RIP. */
        panic_emergency_halt();
    }
    return next_rsp;
}
