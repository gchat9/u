ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
    /*. = 0x08048000 + SIZEOF_HEADERS;*/
    . = 0x00010000 + SIZEOF_HEADERS;
    .text  : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .data  : { *(.data .data.*) } :text
    .bss   : { *(.bss .bss.* COMMON) } :text

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.gnu*)
        *(.eh_frame*)
    }
}
