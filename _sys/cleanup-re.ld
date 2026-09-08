/* linker script for text + read-only data */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
    . = 0x00010000 + SIZEOF_HEADERS;
    .text  : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text

    /DISCARD/ : {
        *(.data*)
        *(.bss*)
        *(.comment)
        *(.note*)
        *(.gnu*)
        *(.eh_frame*)
    }
}
