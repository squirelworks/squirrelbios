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
    /* What the OS keeps after ExitBootServices (memory.c): the runtime
     * services' code, then their data with the system table, the
     * runtime services table and the configuration tables, in pages of
     * their own. Zero-initialized runtime data is stored in the image. */
    . = ALIGN(4096);
    __rt_code_start = .;
    .rt.text : { *(.rt.text .rt.text.*) }
    . = ALIGN(4096);
    __rt_code_end = .;
    __rt_data_start = .;
    .rt.data : {
        *(.rt.rodata .rt.rodata.*)
        *(.rt.data .rt.data.*)
        *(.rt.bss .rt.bss.*)
        *(.rt.*)
    }
    . = ALIGN(4096);
    __rt_data_end = .;
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
