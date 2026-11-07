; PE32+ headers for the hand-written x86_64-uefi test applications.
; Original code. File layout equals memory layout (4 KB alignment):
; headers at 0, .text at 1000h (code and data), .reloc at 2000h. The
; including file defines IMAGE_BASE, entry, reloc and reloc_end, and ends
; the image at 3000h.

        org 0
mz:     db 'MZ'
        times 0x3C - ($ - $$) db 0
        dd pe
pe:     db 'PE', 0, 0
        dw 0x8664                       ; x64
        dw 2                            ; sections
        dd 0, 0, 0
        dw opt_end - opt
        dw 0x0022                       ; executable, large address aware
opt:    dw 0x20B                        ; PE32+
        db 0, 0
        dd 0x1000, 0, 0                 ; code, data, bss sizes
        dd entry                        ; entry point (RVA)
        dd 0x1000                       ; base of code
        dq IMAGE_BASE
        dd 0x1000, 0x1000               ; section and file alignment
        dw 0, 0, 0, 0, 0, 0
        dd 0
        dd 0x3000                       ; size of image
        dd 0x1000                       ; size of headers
        dd 0
        dw 10                           ; EFI application
        dw 0
        dq 0x10000, 0x1000, 0x10000, 0x1000
        dd 0
        dd 16                           ; data directories
        times 5 dq 0
        dd 0x2000, reloc_end - reloc    ; [5] base relocations
        times 10 dq 0
opt_end:
        db '.text', 0, 0, 0
        dd 0x1000, 0x1000, 0x1000, 0x1000, 0, 0
        dw 0, 0
        dd 0xE0000020
        db '.reloc', 0, 0
        dd reloc_end - reloc, 0x2000, 0x1000, 0x2000, 0, 0
        dw 0, 0
        dd 0x42000040
        times 0x1000 - ($ - $$) db 0

