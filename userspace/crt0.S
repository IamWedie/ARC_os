/*
 * ARC OS user-space entry point.
 * _start calls the user main and, if it ever returns, exits via int 0x80.
 */
.section .text
.globl _start
.type _start, @function
_start:
    /* User flat data selector (0x18 | RPL 3) for all data segments. */
    movq $0x1B, %rax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    call shell_main
    movq $3, %rdi              /* SYS_EXIT */
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %rcx, %rcx
    int $0x80
1:  jmp 1b
    .size _start, . - _start