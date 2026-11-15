/* Layout of the device-tree firmware's stand-in EFI applications: one
 * section from 4 KB above the image base, file layout equal to memory
 * layout once tests/mkpe.py adds the headers. Original code. */
ENTRY(efi_main)
SECTIONS
{
    __image_base = 0x10000000;
    . = 0x10001000;
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.sbss .sbss.*)
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
