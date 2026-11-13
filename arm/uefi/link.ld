/* SquirrelBIOS aarch64-uefi: the firmware, linked to run from RAM at
 * FW_BASE. start.S copies it there from flash. Original code. */
OUTPUT_FORMAT("elf64-littleaarch64")
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
    . = ALIGN(16);
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    __fw_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
