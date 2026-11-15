/* SquirrelBIOS sparc64-sun4u: linked at FW_VBASE (0xf0000000), where
 * start.S maps the copy it makes in the top 4 MB of RAM. Original code. */
OUTPUT_FORMAT("elf64-sparc")
OUTPUT_ARCH(sparc:v9)
ENTRY(_start)

SECTIONS
{
    . = 0xf0000000;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
        . = ALIGN(32768);
        KEEP(*(.text.traps))
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(8);
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __fw_end = .;
    ASSERT(__fw_end - 0xf0000000 <= 0x400000, "sparc64: firmware past its 4 MB page")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
