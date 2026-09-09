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
    /* Two PT_LOAD segments with different permissions must be separated
     * by a real page (CONSTANT(MAXPAGESIZE), i.e. whatever -z max-page-size
     * was passed at link time) - not the tiny sub-page alignment the
     * text-only script gets away with - or the kernel ends up mapping
     * them onto the same physical page with only one of the two
     * protections actually applied. See _mk/common.mk: the RW := 1 path
     * passes a real page size here and drops --nmagic, since --nmagic
     * is what would otherwise suppress the file padding this alignment
     * needs to stay in sync with the memory address. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data  : { *(.data .data.*) } :data
    .bss   : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.gnu*)
        *(.eh_frame*)
    }
}
