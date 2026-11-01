; Smoke-test: INT 10h text services beyond teletype.
; Original code.
;
; Sets/gets the cursor, queries the mode, scrolls a window clear, and
; confirms AH=0x0E still writes. Prints vid=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
VGA_SEG         equ 0xB800

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov si, msg_entry
    call puts

    ; Unknown AH -> CF
    mov ah, 0x99
    int 0x10
    jnc .bad

    ; Set cursor to row 2, col 5
    mov ah, 0x02
    xor bh, bh
    mov dh, 2
    mov dl, 5
    int 0x10
    jc .bad

    ; Get cursor back
    mov ah, 0x03
    xor bh, bh
    int 0x10
    jc .bad
    cmp dh, 2
    jne .bad
    cmp dl, 5
    jne .bad

    ; Get mode: AL=3, AH=80, BH=0
    mov ah, 0x0F
    int 0x10
    jc .bad
    cmp al, 3
    jne .bad
    cmp ah, 80
    jne .bad
    test bh, bh
    jnz .bad

    ; Write a marker with teletype at the cursor, then clear the screen
    mov ah, 0x0E
    mov al, 'V'
    int 0x10
    jc .bad
    ; Cell at (5,2) should be 'V'
    mov ax, VGA_SEG
    mov es, ax
    mov di, 2 * 160 + 5 * 2
    cmp byte [es:di], 'V'
    jne .bad

    ; Scroll up, blank whole screen (AL=0)
    mov ax, 0x0600
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F              ; row 24, col 79
    int 0x10
    jc .bad
    cmp byte [es:di], ' '
    jne .bad

    ; Scroll down one full screen line (puts a blank on row 0)
    mov ax, 0x0701
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    jc .bad

    mov si, msg_ok
    jmp .done
.bad:
    mov si, msg_bad
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
msg_ok    db 'vid=ok', 0
msg_bad   db 'vid=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
