/*
 * ARC OS Standalone BIOS Bootloader
 * 16-bit real mode -> 64-bit long mode
 * Loads kernel from disk and jumps to it
 */

.section .text
    .code16
    .global _start
    .global _boot_signature

_start:
    /* Set up segments and stack */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp

    /* Load kernel from disk to 0x100000 */
    movw $0x0010, %cx       /* 16 sectors */
    movw $0x1000, %bx       /* load address */
    movb $0x02, %ah         /* BIOS read sectors */
    movb $0x00, %al
    movb $0x01, %ch         /* cylinder 0 */
    movb $0x02, %cl         /* sector 2 */
    movb $0x00, %dh         /* head 0 */
    int $0x13
    jc .disk_error

    /* Switch to protected mode */
    cli
    lgdt gdt_descriptor

    /* Enable A20 */
    movw $0x2401, %ax
    int $0x15

    /* Enable PAE */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    /* Set up page directory */
    movl $pd, %eax
    movl %eax, %cr3

    /* Enable long mode via EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* Enable paging */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    /* Far jump to 64-bit code segment */
    ljmp $0x08, $1f

1:
    .code64

    /* Set data segments */
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Set up stack */
    movq $stack_top, %rsp

    /* Jump to kernel at 0x100000 */
    movq $0x100000, %rax
    jmp *%rax

.disk_error:
    movw $0x0E, %ax
    movw $0x4F, %dx
    int $0x10
    hlt
    jmp .disk_error

gdt:
    .quad 0x0000000000000000
    .quad 0x0020980000000000
    .quad 0x0000900000000000
gdt_descriptor:
    .word gdt_descriptor - gdt - 1
    .quad gdt

.section .data
.align 4096
pd:
    .long 0x00000083
    .long 0x00000083
    .long 0x00000083
    .long 0x00000083
    .space 4096 - 16

.section .bss
    .space 16384
stack_bottom:
    .space 4096
stack_top:

/* Boot signature at end of 512-byte sector */
    .section .text
    .org 510
_boot_signature:
    .word 0xAA55
