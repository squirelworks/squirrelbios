; Smoke-test: BIOS data area fields after hand-off.
; Original code.
;
; Checks equipment list, conventional memory size (KB), and that the
; page-0 cursor INT 10h uses is still inside 80x25. Prints bda=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
BDA_EQUIP       equ 0x0410
BDA_MEMSIZE     equ 0x0413
BDA_CURSOR      equ 0x0450
BDA_EQUIP_VAL   equ 0x0220
BDA_MEM_KB      equ 640
VGA_COLS        equ 80
VGA_ROWS        equ 25

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov si, msg_entry
    call puts

    mov si, msg_bad
    cmp word [BDA_EQUIP], BDA_EQUIP_VAL
    jne .done
    cmp word [BDA_MEMSIZE], BDA_MEM_KB
    jne .done
    mov ax, [BDA_CURSOR]        ; AL = column, AH = row
    cmp al, VGA_COLS
    jae .done
    cmp ah, VGA_ROWS
    jae .done
    mov si, msg_ok
.done:
    call puts
    mov si, msg_crlf
    call puts

halt:
    cli
    hlt
    jmp halt

puts:
    lodsb
    test al, al
    jz .ret
    call putc
    jmp puts
.ret:
    ret

putc:
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    ret

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'bda=ok', 0
msg_bad   db 'bda=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
