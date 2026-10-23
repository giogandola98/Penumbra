/* versatilepb: RAM at 0, so the vector table is linked at address 0 (low vectors). */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .vectors : { *(.vectors) }
    .text    : { *(.text) }
    .rodata  : { *(.rodata*) }
    .data    : { *(.data) }
    .bss     : { *(.bss) }
}
