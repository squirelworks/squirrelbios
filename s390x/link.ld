/* SquirrelBIOS s390x: a static PIE linked at 0. QEMU loads the -bios
 * ELF 2 MB or so below the top of the first 2 GB of RAM (adding that
 * address to every segment) and starts it with a 64-bit PSW; start.S
 * applies the relocations. Original code. */
OUTPUT_FORMAT("elf64-s390")
OUTPUT_ARCH(s390:64-bit)
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    .plt : { *(.plt) *(.iplt) }
    .rodata : { *(.rodata .rodata.*) }
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .data : { *(.data .data.*) }
    .got : { *(.got.plt) *(.igot.plt) *(.got) *(.igot) }
    . = ALIGN(8);
    .rela.dyn : {
        __rela_start = .;
        *(.rela.*)
        __rela_end = .;
    }
    .dynamic : { *(.dynamic) }
    . = ALIGN(8);
    .bss : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    _end = .;
    ASSERT(_end - _start <= 0x200000, "s390x: firmware past its 2 MB region")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
