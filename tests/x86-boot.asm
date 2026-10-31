; Smoke-test boot sector for SquirrelBIOS x86.
; Stands in for the OS: checks the hand-off state and reports on COM1,
; which the firmware has already programmed.
; Original code.

BITS 16
ORG 0x7C00

COM1 equ 0x3F8

start:
    mov bx, dx                  ; keep DL from the firmware
    mov bp, di                  ; keep DI from the firmware

    mov si, msg_entry
    call puts

    mov si, msg_dl_ok
    cmp bl, 0x80
    je .dl
    mov si, msg_dl_bad
.dl:
    call puts

    mov si, msg_mm_ok
    cmp bp, 0x0500
    jne .mm_bad
    cmp dword [0x0500], 'SQMM'
    jne .mm_bad
    cmp word [0x0504], 0
    jne .mm
.mm_bad:
    mov si, msg_mm_bad
.mm:
    call puts

.halt:
    cli
    hlt
    jmp .halt

puts:
    lodsb
    test al, al
    jz .done
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    jmp puts
.done:
    ret

msg_entry  db 'OS-ENTRY x86', 0
msg_dl_ok  db ' dl=ok', 0
msg_dl_bad db ' dl=bad', 0
msg_mm_ok  db ' memmap=ok', 13, 10, 0
msg_mm_bad db ' memmap=bad', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
