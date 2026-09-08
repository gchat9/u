/* linker script for text, ro data, rw data, and bss */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00010000 + SIZEOF_HEADERS;
    .text  : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    /*. = ALIGN(0x400);*/
    .data  : { *(.data .data.*) } :data
    .bss   : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.gnu*)
        *(.eh_frame*)
    }
}
