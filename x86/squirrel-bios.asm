; Squirrel-OS/2 — minimal firmware ROM for QEMU
; 64 KB image. Reset vector at offset 0xFFF0.
; Original code.
;
; Build:  nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
; Run:    qemu-system-i386 -bios squirrel-bios.rom
;
; Current behaviour: print banner to VGA text, then halt.
; Next step: read sector 1 of the first disk into 0x7C00 and jump there.

BITS 16

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Banner on VGA text (white on black)
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov si, banner
    mov ah, 0x0F
.print:
    lodsb
    test al, al
    jz .done
    stosw
    jmp .print
.done:
    ; Halt until we add the disk hand-off
.halt:
    hlt
    jmp .halt

banner db 'Squirrel-OS/2 ROM', 0

; Pad so the reset vector lands at offset 0xFFF0
times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

; Pad the remainder of the 64 KB image
times 65536 - ($ - $$) db 0
