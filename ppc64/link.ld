/* SquirrelBIOS ppc64le-powernv: linked to run at FW_BASE (start.S copies
 * it there from physical 0). Original code. */
OUTPUT_FORMAT("elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)
ENTRY(_start)

SECTIONS
{
    . = FW_BASE;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(256);
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .toc : { *(.toc) *(.got) }
    . = ALIGN(8);
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __fw_end = .;
    ASSERT(__fw_end - _start <= 0x400000, "ppc64: firmware past its 4 MB region")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
