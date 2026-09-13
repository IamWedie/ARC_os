ENTRY(_entry64)
SECTIONS
{
    . = 0x200000;

    .text : {
        *(.entry64)
        *(.text)
        *(.rodata*)
    }

    .data : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        . = ALIGN(16);
        __bss_end = .;
        . = ALIGN(16);
        . = . + 0x10000;
        __stack_top = .;
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
        *(.rela*)
        *(.e_frame_hdr)
    }
}