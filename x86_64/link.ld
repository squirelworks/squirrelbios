/* SquirrelBIOS x86_64-uefi: the firmware body, run from RAM at FW_BASE.
 * Original code. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = FW_BASE;
    __fw_start = .;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    __fw_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
