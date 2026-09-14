/*
 * ARC OS Long-mode interrupt stubs.
 * IRQ entry via the IDT pushes (SS, RSP, RFLAGS, CS, RIP); these stubs save
 * ALL caller- and callee-saved GPRs, dispatch to the C handler, restore and
 * return with iretq so the CPU state is correctly restored. The callee-saved
 * regs (rbx, rbp, r12-r15) must be saved too: the interrupted thread may be
 * preempted (timer) or trapped into the kernel and must resume with its own
 * values, not whatever the kernel C code left behind.
 * AT&T syntax (GNU assembler via zig cc).
 *
 * irq0_stub is also the preemption point: it passes its own stack pointer to
 * irq0_handler and, if the scheduler picks another process, loads that
 * process's saved stack pointer before POPREGS/iretq.
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
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POPREGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
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
    movq %rsp, %rdi
    call irq0_handler
    testq %rax, %rax
    jz 1f
    movq %rax, %rsp
1:
    POPREGS
    iretq

.globl irq1_stub
.type irq1_stub, @function
irq1_stub:
    PUSHREGS
    call irq1_handler
    POPREGS
    iretq

/*
 * Local APIC timer preemption point (vector 0x40). Mirrors irq0_stub:
 * passes its stack pointer to apic_timer_handler and context switches on
 * the returned stack pointer if the scheduler picked another process.
 */
.globl apic_timer_stub
.type apic_timer_stub, @function
apic_timer_stub:
    PUSHREGS
    movq %rsp, %rdi
    call apic_timer_handler
    testq %rax, %rax
    jz 1f
    movq %rax, %rsp
1:
    POPREGS
    iretq

/*
 * int 0x80 syscall gate (DPL 3). A privilege transition happens on entry so
 * the kernel stack is auto-selected via TSS RSP0; the user's SysV args are
 * still in rdi/rsi/rdx/rcx when we arrive. We forward them to the C
 * dispatcher, park the return value in a placeholder slot pushed above the
 * saved registers, restore the user's registers and only then load the
 * return value into rax so iretq delivers it to the caller.
 */
.globl syscall_stub
.type syscall_stub, @function
syscall_stub:
    pushq $0                /* return-value slot (popped into rax below) */
    PUSHREGS
    call syscall_dispatch
    movq %rax, 15*8(%rsp)   /* park result above the saved regs */
    POPREGS
    popq %rax
    iretq