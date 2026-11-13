/* SquirrelBIOS riscv64-uefi: the UEFI core, linked at FW_BASE (2 MB into
 * RAM, right after the region the SBI keeps to itself). The image file
 * puts it there: the SBI, padded to 2 MB, then this. Original code. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = FW_BASE;
    __fw_start = .;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    . = ALIGN(16);
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    __fw_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
