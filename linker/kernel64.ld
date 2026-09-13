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

    __kernel_start = 0x100000;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
        *(.rela*)
        *(.e_frame_hdr)
    }
}