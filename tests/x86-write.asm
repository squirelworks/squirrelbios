; Smoke-test: INT 13h AH=03 / AH=43 disk writes.
; Original code.
;
; Writes a pattern to LBA 1 with extended write, reads it back; writes
; another pattern to LBA 2 with CHS write, reads it back. Prints write=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
BUF1            equ 0x8000
BUF2            equ 0x8200
PAT1            equ 0xA5
PAT2            equ 0x5A

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg_entry
    call puts

    ; Fill BUF1 with PAT1
    mov di, BUF1
    mov cx, 512
    mov al, PAT1
    rep stosb

    ; Extended write LBA 1
    mov si, dap1
    mov dl, 0x80
    mov ah, 0x43
    int 0x13
    jc .bad
    test ah, ah
    jnz .bad

    ; Clear BUF2, then extended read LBA 1 into it
    mov di, BUF2
    mov cx, 512
    xor al, al
    rep stosb
    mov si, dap1r
    mov dl, 0x80
    mov ah, 0x42
    int 0x13
    jc .bad
    mov di, BUF2
    mov cx, 512
.chk1:
    cmp byte [di], PAT1
    jne .bad
    inc di
    loop .chk1

    ; Fill BUF1 with PAT2; CHS write to LBA 2
    ; LBA 2 = C=0 H=0 S=3 with SPT=63
    mov di, BUF1
    mov cx, 512
    mov al, PAT2
    rep stosb
    mov ax, 0x0301              ; write 1 sector
    mov cx, 0x0003              ; cyl 0, sector 3
    mov dx, 0x0080              ; head 0, drive 0x80
    mov bx, BUF1
    int 0x13
    jc .bad
    test ah, ah
    jnz .bad

    ; CHS read LBA 2 into BUF2
    mov di, BUF2
    mov cx, 512
    xor al, al
    rep stosb
    mov ax, 0x0201
    mov cx, 0x0003
    mov dx, 0x0080
    mov bx, BUF2
    int 0x13
    jc .bad
    mov di, BUF2
    mov cx, 512
.chk2:
    cmp byte [di], PAT2
    jne .bad
    inc di
    loop .chk2

    cli
    mov si, msg_ok
    jmp .done
.bad:
    cli
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

align 16
dap1:
    db 0x10, 0
    dw 1
    dw BUF1, 0
    dd 1, 0                     ; LBA 1
dap1r:
    db 0x10, 0
    dw 1
    dw BUF2, 0
    dd 1, 0

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'write=ok', 0
msg_bad   db 'write=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
; Extra sectors so LBA 1 and 2 exist on the disk image
times 512 * 3 db 0
