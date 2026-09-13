/*
 * TinyOS - Multiboot1 entry point (for GRUB)
 * Calls kernel_main after switching to 64-bit mode
 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

.section .bss
    .space 16384
stack_bottom:
    .space 4096
stack_top:

.section .text
    .code32
    .global _start

_start:
    movl $stack_top, %esp
    movl %eax, %ebx       /* Save multiboot info */

    /* Load GDT */
    lgdt gdt_descriptor

    /* Enable PAE */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    /* Set up page directory */
    movl $pd, %eax
    movl %eax, %cr3

    /* Enable long mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* Enable paging */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    /* Far jump to 64-bit mode */
    ljmp $0x08, $1f

1:
    .code64
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movq $stack_top, %rsp

    /* Load IDT */
    lidt idt_descriptor

    /* Call kernel_main(multiboot_info) */
    movq %rbx, %rdi
    call kernel_main

.halt:
    hlt
    jmp .halt

gdt:
    .quad 0x0000000000000000
    .quad 0x0020980000000000
    .quad 0x0000900000000000
gdt_descriptor:
    .word gdt_descriptor - gdt - 1
    .quad gdt

    .space 256 * 16
idt_descriptor:
    .word 0
    .quad 0

.section .data
.align 4096
pd:
    .long 0x00000083
    .long 0x00000083
    .long 0x00000083
    .long 0x00000083
    .space 4096 - 16
