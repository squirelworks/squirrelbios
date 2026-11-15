/* SquirrelBIOS m68k-virt: QEMU's virt machine has no ROM, so the
 * firmware is an ELF that QEMU loads (-kernel) at FW_BASE and starts.
 * QEMU puts its bootinfo records right after the image's end (_end).
 * Original code. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

SECTIONS
{
    . = FW_BASE;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(4);
    .bss : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    _end = .;
    ASSERT(_end - _start <= 0x100000, "m68k: firmware past its 1 MB region")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
