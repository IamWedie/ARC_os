/* ARC OS user-space linker script: fixed 64-bit flat-mapped image. */
ENTRY(_start)

SECTIONS {
    . = 0x8000000000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) }

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) *(.rela*) *(.rel*) }
}