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

.globl ata_irq_stub
.type ata_irq_stub, @function
ata_irq_stub:
    PUSHREGS
    call ata_irq_handler
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
 * Exception stubs (vectors 0..31). The CPU pushes RIP/CS/RFLAGS/[RSP/SS]
 * and, for the error-code vectors 8,10,11,12,13,14,17,30, an extra error
 * code. To give the C handler one uniform layout we keep the hardware error
 * code (error-code vectors) or push a dummy 0 (the rest), then push the
 * vector number under it. Stack after PUSHREGS (rsp -> r15 ... rax,
 * +120 vector, +128 error, +136 RIP, +144 CS, +152 RFLAGS, +160 RSP,
 * +168 SS when a privilege change occurred). Never returns: the handler
 * halts after dumping context.
 */
.macro EXC_STUB num, has_err
    .globl exc_stub_\num
    .type  exc_stub_\num, @function
exc_stub_\num:
.if \has_err
    pushq $\num        /* hardware error code is already below */
.else
    pushq $0           /* dummy error code for a uniform layout */
    pushq $\num
.endif
    PUSHREGS
    movq %rsp, %rdi
    call exception_handler
1:  cli
    hlt
    jmp 1b
.endm

/* error-code vectors: 8, 10, 11, 12, 13, 14, 17, 30 */
EXC_STUB 0,0
EXC_STUB 1,0
EXC_STUB 2,0
EXC_STUB 3,0
EXC_STUB 4,0
EXC_STUB 5,0
EXC_STUB 6,0
EXC_STUB 7,0
EXC_STUB 8,1
EXC_STUB 9,0
EXC_STUB 10,1
EXC_STUB 11,1
EXC_STUB 12,1
EXC_STUB 13,1
EXC_STUB 14,1
EXC_STUB 15,0
EXC_STUB 16,0
EXC_STUB 17,1
EXC_STUB 18,0
EXC_STUB 19,0
EXC_STUB 20,0
EXC_STUB 21,0
EXC_STUB 22,0
EXC_STUB 23,0
EXC_STUB 24,0
EXC_STUB 25,0
EXC_STUB 26,0
EXC_STUB 27,0
EXC_STUB 28,0
EXC_STUB 29,0
EXC_STUB 30,1
EXC_STUB 31,0

/* Address table used by C to install the IDT gates for vectors 0..31. */
.section .data
.globl exc_stub_table
.align 8
exc_stub_table:
    .quad exc_stub_0,  exc_stub_1,  exc_stub_2,  exc_stub_3
    .quad exc_stub_4,  exc_stub_5,  exc_stub_6,  exc_stub_7
    .quad exc_stub_8,  exc_stub_9,  exc_stub_10, exc_stub_11
    .quad exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15
    .quad exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19
    .quad exc_stub_20, exc_stub_21, exc_stub_22, exc_stub_23
    .quad exc_stub_24, exc_stub_25, exc_stub_26, exc_stub_27
    .quad exc_stub_28, exc_stub_29, exc_stub_30, exc_stub_31
.section .text

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