/*
 * ARC OS - 64-bit kernel entry point.
 * Entered in long mode with paging active (identity map of first 512MB).
 * RDI = multiboot info address (from boot32.S).
 * Jumps to kernel_main().
 */
.section .entry64, "ax"
    .code64
    .globl _entry64

_entry64:
    leaq str_enter(%rip), %rsi
    call com1_puts64
    leaq str_e_skip(%rip), %rsi
    call com1_puts64

    /* Zero BSS */
    movq $__bss_start, %rax
    movq $__bss_end, %rcx
1:
    cmpq %rcx, %rax
    jae 2f
    movb $0, (%rax)
    incq %rax
    jmp 1b
2:

    /* Set up stack */
    movq $__stack_top, %rsp

    /* Call kernel_main(multiboot_addr) */
    call kernel_main

.hlt_loop:
    hlt
    jmp .hlt_loop

/* 64-bit serial debug (COM1 already initialized by boot32) */
com1_puts64:
    movb (%rsi), %al
    testb %al, %al
    jz 2f
1:  movw $0x3fd, %dx
3:  inb %dx, %al
    testb $0x20, %al
    jz 3b
    movw $0x3f8, %dx
    movb (%rsi), %al
    outb %al, %dx
    incq %rsi
    movb (%rsi), %al
    testb %al, %al
    jnz 1b
2:  ret

str_e_movs: .asciz "E64:movs\n"
str_e_done: .asciz "E64:segdone\n"
str_e_skip: .asciz "E64:skipseg\n"
str_enter: .asciz "E64:long-mode\n"