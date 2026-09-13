/*
 * ARC OS Long-mode interrupt stubs.
 * IRQ entry via the IDT pushes (SS, RSP, RFLAGS, CS, RIP); these stubs save
 * caller-saved registers, dispatch to the C handler, restore and return with
 * iretq so the CPU state is correctly restored.
 * AT&T syntax (GNU assembler via zig cc).
 */

.section .text

.macro PUSHREGS
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
.endm

.macro POPREGS
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
.endm

.globl irq0_stub
.type irq0_stub, @function
irq0_stub:
    PUSHREGS
    call irq0_handler
    POPREGS
    iretq

.globl irq1_stub
.type irq1_stub, @function
irq1_stub:
    PUSHREGS
    call irq1_handler
    POPREGS
    iretq