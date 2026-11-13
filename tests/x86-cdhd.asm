; Smoke-test: an El Torito hard disk emulation image (media 4) as C:.
; Original code.
;
; tests/mkfloppy.py --hdd makes a 2016-sector image (2 cylinders, 16
; heads, 63 sectors, the geometry this MBR's one partition ends on) with
; this sector at LBA 0 and 'LBA:' + LBA tags in the others; tests/mkiso.py
; --hdd makes it the CD's boot image. Steps:
;   d  DL = 80h
;   p  AH=08h: CH = 1, CL = 63, DH = 15, DL = 1
;   r  AH=02h C0 H0 S2: LBA 1
;   m  AH=02h two sectors from C0 H15 S63: LBA 1007, then C1 H0 S1, 1008
;   x  AH=42h one sector at LBA 2015, the last
;   w  AH=03h: CF set, AH = 03h (write-protected)
;   n  AH=02h C2 H0 S1, past the end: CF set, AH = 04h
;   t  AH=15h: AH = 03h, CX:DX = 2016
;   s  AH=4B01h: media 4, drive 80h
; Prints cdhd=ok, or cdhd=bad:<step> <AH in hex>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
BUF     equ 0x1000

start:
    cld
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    mov bp, dap
    push dx
    mov si, msg_entry
    call puts
    pop dx
    mov byte [bp + step - dap], 'd'
    cmp dl, 0x80
    jne bad

    mov byte [bp + step - dap], 'p'
    mov ah, 0x08
    int 0x13
    jc bad
    cmp cx, 0x013F
    jne bad
    cmp dx, 0x0F01
    jne bad

    mov byte [bp + step - dap], 'r'
    push word BUF
    pop es
    xor bx, bx
    mov ax, 0x0201
    mov cx, 0x0002
    mov dx, 0x0080
    int 0x13
    jc bad
    mov ax, 1
    call tag0
    jne bad

    mov byte [bp + step - dap], 'm'
    mov ax, 0x0202
    mov cx, 0x003F
    mov dx, 0x0F80
    int 0x13
    jc bad
    mov ax, 1007
    call tag0
    jne bad
    cmp word [es:512 + 4], 1008
    jne bad

    mov byte [bp + step - dap], 'x'
    mov ah, 0x42
    mov si, dap
    int 0x13
    jc bad
    mov ax, 2015
    call tag0
    jne bad

    mov byte [bp + step - dap], 'w'
    mov ax, 0x0301
    mov cx, 0x0002
    int 0x13
    jnc bad
    cmp ah, 0x03
    jne bad

    mov byte [bp + step - dap], 'n'
    mov ax, 0x0201
    mov cx, 0x0201
    mov dh, 0
    int 0x13
    jnc bad
    cmp ah, 0x04
    jne bad

    mov byte [bp + step - dap], 't'
    mov ah, 0x15
    int 0x13
    jc bad
    cmp ah, 0x03
    jne bad
    cmp cx, 0
    jne bad
    cmp dx, 2016
    jne bad

    mov byte [bp + step - dap], 's'
    mov ax, 0x4B01
    mov dl, 0x80
    mov si, spec
    int 0x13
    jc bad
    cmp word [spec + 1], 0x8004
    jne bad

    mov si, msg_ok
    call puts
    jmp halt

bad:
    mov bl, ah
    mov si, msg_bad
    call puts
    mov al, [step]
    mov [msg_step], al
    mov al, bl
    shr al, 4
    call hex
    mov [msg_step + 2], al
    mov al, bl
    and al, 0x0F
    call hex
    mov [msg_step + 3], al
    mov si, msg_step
    call puts
halt:
    hlt
    jmp halt

hex:
    add al, '0'
    cmp al, '9'
    jbe .r
    add al, 7
.r:
    ret

; ZF set when ES:0 is the tag of LBA AX
tag0:
    cmp dword [es:0], 'LBA:'
    jne .r
    cmp [es:4], ax
.r:
    ret

puts:
    lodsb
    test al, al
    jz .r
    mov dx, COM1 + 5
    push ax
.w:
    in al, dx
    test al, 0x20
    jz .w
    pop ax
    mov dx, COM1
    out dx, al
    jmp puts
.r:
    ret

dap       db 0x10, 0
          dw 1, 0, BUF
          dd 2015, 0
step      db 0
msg_entry db 'OS-ENTRY x86 cdhd ', 0
msg_ok    db 'cdhd=ok', 13, 10, 0
msg_bad   db 'cdhd=bad:', 0
msg_step  db '? ??', 13, 10, 0
spec:

times 0x1BE - ($ - $$) db 0
    ; One active partition from C0 H1 S1 to C1 H15 S63
    db 0x80, 1, 1, 0, 0x06, 15, 63, 1
    dd 63, 2016 - 63
times 510 - ($ - $$) db 0
dw 0xAA55
