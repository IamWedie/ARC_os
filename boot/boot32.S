/*
 * ARC OS - 32-bit Multiboot1 boot stub.
 * Builds minimal paging, switches to long mode, and jumps to the
 * 64-bit kernel embedded in this image at 0x200000.
 *
 * NOTE: must be linked as a 32-bit ELF (QEMU/GRUB multiboot rejects
 * x86-64 ELF). Use: zig cc -target i386-freestanding
 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.set KERNEL_LOAD, 0x200000      /* load address of embedded 64-bit kernel */

.section .multiboot
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

/* NOTE: the 64-bit kernel blob is embedded at link time via an objcopy
 * binary object (see build_kernel.py). boot32.ld places its
 * `.kernelblob` section at 0x200000. */

.section .text
    .code32
    .globl _start

_start:
    cli

    /* Save multiboot info pointer (ebx) into edi; we pass it to the
     * kernel as the first argument once in long mode. */
    movl %ebx, %edi

    call com1_init
    movl $str_start, %esi
    call com1_puts

    lgdt gdt_descriptor
    movl $str_lgdt, %esi
    call com1_puts

    /* Enable PAE */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4
    movl $str_pae, %esi
    call com1_puts

    /* Load PML4 */
    movl $pml4, %eax
    movl %eax, %cr3
    movl $str_cr3, %esi
    call com1_puts

    /* Enable long mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl $str_lme, %esi
    call com1_puts

    /* Enable paging */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    movl $str_pg, %esi
    call com1_puts

    /* Jump to the embedded 64-bit kernel (now in long mode).
     * RDI = multiboot info addr. */
    ljmp $0x08, $KERNEL_LOAD

/* ---- serial COM1 debug helpers (32-bit) ---- */
com1_init:
    movb $0x00, %al
    movw $0x3f9, %dx
    outb %al, %dx          /* IER = 0 (no interrupts) */
    movb $0x80, %al
    movw $0x3fb, %dx
    outb %al, %dx          /* LCR: DLAB set */
    movb $0x01, %al
    movw $0x3f8, %dx
    outb %al, %dx          /* DLL = 1 (115200) */
    movb $0x00, %al
    movw $0x3f9, %dx
    outb %al, %dx          /* DLM = 0 */
    movb $0x03, %al
    movw $0x3fb, %dx
    outb %al, %dx          /* LCR = 8N1, DLAB clear */
    movb $0xc7, %al
    movw $0x3fa, %dx
    outb %al, %dx          /* FCR: enable FIFO */
    movb $0x0e, %al
    movw $0x3fc, %dx
    outb %al, %dx          /* MCR: RTS + OUT2 */
    ret

com1_puts:
    lodsb
    testb %al, %al
    jz 2f
1:  pushl %eax
    movw $0x3fd, %dx       /* wait for THR empty */
3:  inb %dx, %al
    testb $0x20, %al
    jz 3b
    movw $0x3f8, %dx
    popl %eax
    outb %al, %dx
    lodsb
    testb %al, %al
    jnz 1b
2:  ret

str_start: .asciz "B32:multiboot-entry\n"
str_lgdt:  .asciz "B32:gdt\n"
str_pae:   .asciz "B32:pae\n"
str_cr3:   .asciz "B32:cr3\n"
str_lme:   .asciz "B32:lme\n"
str_pg:    .asciz "B32:paging\n"

gdt:
    .quad 0x0000000000000000   /* null */
    .quad 0x0020980000000000   /* 64-bit code, DPL0 */
    .quad 0x0000900000000000   /* data */
gdt_descriptor:
    .word gdt_descriptor - gdt - 1
    .long gdt

.section .data
.align 4096
pml4:
    .long (pdpt + 0x003)   /* PML4E[0] -> pdpt, present/rw */
    .long 0
    .fill 511, 8, 0
.align 4096
pdpt:
    .long (pd + 0x003)     /* PDPTE[0] -> pd, present/rw */
    .long 0
    .fill 511, 8, 0
.align 4096
pd:
    .set i, 0
    .rept 256              /* identity-map first 512MB (256 * 2MB pages) */
        .long (i << 21) | 0x83   /* present, rw, PS (2MB in PAE) */
        .long 0
        .set i, i + 1
    .endr
    .fill 256, 8, 0

.section .bss
    .align 16
    .space 32768
stack_top: