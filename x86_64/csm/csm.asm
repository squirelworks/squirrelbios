; SquirrelBIOS x86_64-uefi: the CSM's way back to real mode, and the
; legacy x86-pc ROM it starts.
; Original code.
;
; csm_tramp..csm_tramp_end is copied to CSM_TRAMP (below 64 KB) and
; entered there in 64-bit mode with the first 4 GB mapped 1:1. It loads
; its own GDT, drops to 32-bit compatibility mode, turns paging and long
; mode off, goes through a 16-bit protected-mode segment to real mode and
; jumps to F000:FFF0, the reset vector of the legacy ROM that csm.c has
; already copied to shadow RAM at F0000.

        bits 64
        section .rodata
        global csm_tramp, csm_tramp_end, csm_legacy_rom, csm_legacy_rom_end

CSM_TRAMP equ 0x8000
%define ABS(x) (CSM_TRAMP + (x) - csm_tramp)

SEL_CODE32 equ 0x08
SEL_CODE16 equ 0x10
SEL_DATA16 equ 0x18

        align 16
csm_tramp:
        cli
        lgdt [ABS(gdtr)]
        push qword SEL_CODE32
        push qword ABS(compat32)
        o64 retf

        bits 32
compat32:
        mov eax, cr0
        and eax, 0x7FFFFFFF         ; paging off: leaves long mode
        mov cr0, eax
        mov ecx, 0xC0000080         ; EFER
        rdmsr
        and eax, ~0x100             ; LME off
        wrmsr
        xor eax, eax
        mov cr4, eax
        mov cr3, eax
        jmp SEL_CODE16:ABS(pm16)

        bits 16
pm16:
        mov ax, SEL_DATA16          ; 64 KB limits, as real mode expects
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov eax, cr0
        and eax, 0x7FFFFFFE         ; protection off (paging is already)
        mov cr0, eax
        jmp 0:ABS(real16)
real16:
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov sp, CSM_TRAMP
        lidt [ABS(ivt)]
        jmp 0xF000:0xFFF0           ; the legacy ROM's reset vector

        align 8
gdt:
        dq 0
        dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code, 4 GB
        dq 0x00009A000000FFFF       ; 0x10: 16-bit code, 64 KB
        dq 0x000092000000FFFF       ; 0x18: 16-bit data, 64 KB
gdtr:
        dw 4 * 8 - 1
        dq ABS(gdt)                 ; lgdt in 64-bit mode reads 8 bytes
ivt:
        dw 0x3FF                    ; the real-mode interrupt vectors at 0
        dd 0
csm_tramp_end:

        align 16
csm_legacy_rom:
        incbin LEGACY_ROM
csm_legacy_rom_end:

        section .note.GNU-stack noalloc noexec nowrite progbits
