; SquirrelBIOS x86 — hand-off to the boot sector and runtime set-up
; Original code. Included by x86/squirrel-bios.asm.

handoff:
    call prepare_runtime
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
    mov dl, [ss:BOOT_DL]        ; 0x80 master or 0x81 slave
    mov di, MEMMAP_ADDR         ; memory map header
    jmp 0x0000:0x7C00

; A20, APIC virtual-wire, PIT, PIC, IVT. IF stays clear; IRQ0 is unmasked
; so a later STI sees the timer. Called once on every hand-off path.
prepare_runtime:
    call enable_a20
    call apic_virtual_wire
    call pit_pic_init
    call install_ivt
%if CONFIG_OPTROM
    call optrom_scan
%endif
    ret

; Scan legacy option ROM space 0xD0000–0xEFFFF (skip VGA at 0xC0000).
; Each 2 KB: 0x55 0xAA, size byte (512-byte units), checksum 0 over the
; ROM, then far-call offset 3. Bad signatures or checksums are skipped.
OPTROM_SEG_FIRST equ 0xD000
OPTROM_SEG_LAST  equ 0xF000          ; stop before the system BIOS
