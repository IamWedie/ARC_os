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
     * KEEP is required: the blob is only referenced from this script, so
     * --gc-sections would otherwise drop it. QEMU multiboot loads this
     * segment into RAM at 0x200000. */
    .kernel 0x200000 : {
        KEEP(*(.kernelblob))
    }
}