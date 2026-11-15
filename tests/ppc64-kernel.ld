/* Smoke-test kernel for ppc64le-powernv, linked (and loaded) at KBASE
 * (--defsym KBASE=...). Original code. */
OUTPUT_FORMAT("elf64-powerpcle")
OUTPUT_ARCH(powerpc:common64)
ENTRY(_start)

SECTIONS
{
    . = KBASE;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .toc : { *(.toc) *(.got) }
    .bss : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
