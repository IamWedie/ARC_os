ENTRY(_start)
SECTIONS
{
    . = 0x100000;

    .text : {
        *(.multiboot)
        *(.text)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(.bss)
    }

    /* Embedded 64-bit kernel blob at its load address.
     * QEMU multiboot loads this segment into RAM at 0x200000. */
    .kernel 0x200000 : {
        *(.kernelblob)
    }
}