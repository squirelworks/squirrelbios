; Boot sector that reads the way Squirrel-OS/2's boot sector does.
; Original code.
;
; No SQBL header and no S2OK signature, so the firmware enters 0x7C00
; instead of loading the image itself. The sector then:
;   - rejects a short disk-address packet (CF set, AH=01h)
;   - reads LBAs 1..192 with three INT 13h AH=42 calls of 64 sectors
;   - checks INT 13h AH=08 (63 sectors, 16 heads)
;   - reads LBA 1 again with AH=02 (CHS) and AH=00 (reset)
;   - rejects a read of a drive that is not 0x80 or 0x81
;   - writes 'T', CR, LF, 'U' with INT 10h AH=0E and reads the cells back
;
; The bytes those reads must find live in the sectors after this one.
; The last dword of LBA 192 is 'I13!', not 'S2OK'.

BITS 16

section boot vstart=0x7C00

COM1 equ 0x3F8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov si, msg_via
    call puts

    mov si, badpkt
    mov dl, 0x80
    mov ah, 0x42
    int 0x13
    mov al, 'P'
    jnc fail
    cmp ah, 0x01
    jne fail

    mov si, dap
    mov cx, 3
.next:
    mov dl, 0x80
    mov ah, 0x42
    int 0x13
    mov al, 'E'
    jc fail
    test ah, ah
    jnz fail
    add word [dap + 6], 0x0800      ; next 32 KB, in paragraphs
    add dword [dap + 8], 64
    loop .next

    mov al, 'M'
    cmp byte [0x8000], 0x11
    jne fail
    mov ax, 0x1000
    mov es, ax
    cmp byte [es:0], 0x22
    jne fail
    mov ax, 0x1FFF
    mov es, ax
    cmp dword [es:0x000C], 'I13!'
    jne fail

    xor di, di
    mov es, di
    mov dl, 0x80
    mov ah, 0x08
    int 0x13
    mov al, 'G'
    jc fail
    test ah, ah
    jnz fail
    cmp dh, 15
    jne fail
    and cl, 0x3F
    cmp cl, 63
    jne fail

    ; LBA 1 is cylinder 0, head 0, sector 2 with that geometry.
    mov ax, 0x2000
    mov es, ax
    xor bx, bx
    mov ax, 0x0201
    mov cx, 0x0002
    mov dx, 0x0080
    int 0x13
    jc .chsfail
    cmp ax, 0x0001
    jne .chsfail
    cmp byte [es:0], 0x11
    jne .chsdata
    jmp .chsok
.chsfail:
    mov al, 'C'
    jmp fail
.chsdata:
    mov al, 'c'
    jmp fail
.chsok:

    xor ah, ah
    mov dl, 0x80
    int 0x13
    mov al, 'R'
    jc fail
    test ah, ah
    jnz fail

    mov ax, 0x0201
    mov cx, 0x0001
    mov dx, 0x0082                  ; neither master nor slave
    xor bx, bx
    int 0x13
    mov al, 'D'
    jnc fail
    cmp ah, 0x01
    jne fail

    mov si, msg_tty
.tty:
    lodsb
    test al, al
    jz .ttydone
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp .tty
.ttydone:
    mov ax, 0xB800
    mov es, ax
    mov al, 'T'
    cmp byte [es:0], 'T'
    jne fail
    cmp byte [es:160], 'U'
    jne fail

    mov si, msg_ok
    call puts
    jmp halt

fail:
    mov bl, al
    mov bh, ah
    mov si, msg_bad
    call puts
    mov al, bl
    call putc
    mov al, bh
    call hex8
    mov al, 13
    call putc
    mov al, 10
    call putc

halt:
    cli
    hlt
    jmp halt

puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
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

hex8:
    push cx
    mov cl, al
    shr al, 4
    call .nib
    mov al, cl
    and al, 0x0F
    call .nib
    pop cx
    ret
.nib:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 'A' - '9' - 1
    jmp putc

msg_via db 'via-boot-sector', 13, 10, 0
msg_ok  db 'int13=ok tty=ok', 13, 10, 0
msg_bad db 'int13=bad-', 0
msg_tty db 'T', 13, 10, 'U', 0
badpkt  db 0
dap:
    db 0x10, 0
    dw 64
    dw 0x8000, 0x0000               ; 0x0000:0x8000
    dq 1

times 510 - ($ - $$) db 0
dw 0xAA55

; LBAs 1..192. 0x11 starts the first AH=42, 0x22 the second, 'I13!' ends
; the third. 'I13!' is also what keeps this off the S2OK fast path.
section payload
db 0x11
times 64 * 512 - 1 db 0
db 0x22
times 128 * 512 - 5 db 0
dd 'I13!'
