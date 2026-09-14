ENTRY(_start)
SECTIONS
{
    . = 0x100000;

    .bootdata ALIGN(4K) : {
        *(.multiboot)
        *(.note.Xen)
        *(.rodata.str1.1)
        *(.rodata)
        *(.eh_frame_hdr)
        *(.eh_frame)
    }

    .text ALIGN(4K) : {
        *(.text)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }
}
