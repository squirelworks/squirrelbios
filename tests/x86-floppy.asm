; Smoke-test: boot from a 1.44 MB diskette and use INT 13h on it.
; Original code.
;
; tests/mkfloppy.py puts this sector at C0 H0 S1 of a 1.44 MB image and
; tags every other sector with 'LBA:' and its LBA. The boot sector checks:
;   d  DL = 0x00 (drive A:)
;   p  AH=08h: CF clear, CH = 79, CL = 18, DH = 1, DL >= 1, ES:DI equals
;      the INT 1Eh vector and the table says 18 sectors per track
;   r  AH=02h one sector (C0 H0 S2)
;   m  AH=02h three sectors from C0 H1 S17, which runs on to C1 H0 S1
;   w  AH=03h writes C2 H0 S5, AH=02h reads it back (built with -DCDEMU,
;      as the diskette image of an El Torito CD: the write is refused
;      with AH = 03h, write-protected)
;   n  sector 19 does not exist: CF set, AH = 04h
;   t  AH=15h: drive A: is a diskette (AH = 1)
; Prints floppy=ok, or floppy=bad:<letter> <AH in hex>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
BUF     equ 0x1000                  ; segment of the read buffer
WBUF    equ 0x2000                  ; segment of the write buffer

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [drive], dl
    mov si, msg_entry
    call puts

    mov byte [step], 'd'
    cmp byte [drive], 0x00
    jne bad

    ; --- AH=08h ---
    mov byte [step], 'p'
    mov ah, 0x08
    mov dl, 0
    int 0x13
    jc bad
    cmp ch, 79
    jne bad
    cmp cl, 18
    jne bad
    cmp dh, 1
    jne bad
    cmp dl, 1
    jb bad
    cmp di, [0x1E * 4]
    jne bad
    mov ax, es
    cmp ax, [0x1E * 4 + 2]
    jne bad
    cmp byte [es:di + 4], 18
    jne bad

    ; --- one sector ---
    mov byte [step], 'r'
    mov ax, BUF
    mov es, ax
    xor bx, bx
    mov ax, 0x0201
    mov cx, 0x0002                  ; C0, S2
    mov dx, 0x0000                  ; H0, A:
    int 0x13
    jc bad
    cmp al, 1
    jne bad
    xor di, di
    mov ax, 1
    call check_tag
    jne bad

    ; --- three sectors across a head and a cylinder ---
    mov byte [step], 'm'
    xor bx, bx
    mov ax, 0x0203
    mov cx, 0x0011                  ; C0, S17
    mov dx, 0x0100                  ; H1, A:
    int 0x13
    jc bad
    cmp al, 3
    jne bad
    xor di, di
    mov ax, 18 + 16                 ; C0 H1 S17
    call check_tag
    jne bad
    mov di, 512
    mov ax, 18 + 17                 ; C0 H1 S18
    call check_tag
    jne bad
    mov di, 1024
    mov ax, 36                      ; C1 H0 S1
    call check_tag
    jne bad

    ; --- write, then read back ---
    mov byte [step], 'w'
%ifdef CDEMU
    xor bx, bx
    mov ax, 0x0301
    mov cx, 0x0205
    xor dx, dx
    int 0x13
    jnc bad
    cmp ah, 0x03
    jne bad
%else
    mov ax, WBUF
    mov es, ax
    xor di, di
    mov cx, 256
    mov ax, 0xA55A
    rep stosw
    xor bx, bx
    mov ax, 0x0301
    mov cx, 0x0205                  ; C2, S5
    mov dx, 0x0000
    int 0x13
    jc bad
    mov ax, BUF
    mov es, ax
    xor bx, bx
    mov word [es:0], 0
    mov ax, 0x0201
    mov cx, 0x0205
    mov dx, 0x0000
    int 0x13
    jc bad
    push ds
    mov ax, WBUF
    mov ds, ax
    xor si, si
    xor di, di
    mov cx, 256
    repe cmpsw
    pop ds
    jne bad
%endif

    ; --- a sector that does not exist ---
    mov byte [step], 'n'
    xor bx, bx
    mov ax, 0x0201
    mov cx, 0x0013                  ; S19
    mov dx, 0x0000
    int 0x13
    jnc bad
    cmp ah, 0x04
    jne bad

    ; --- drive type ---
    mov byte [step], 't'
    mov ah, 0x15
    mov dl, 0
    int 0x13
    jc bad
    cmp ah, 1
    jne bad

    mov si, msg_ok
    call puts
    jmp halt

bad:
    mov [status], ah
    mov si, msg_bad
    call puts
    mov al, [step]
    call putc
    mov al, ' '
    call putc
    mov al, [status]                ; AH at the failure, in hex
    shr al, 4
    call hexdigit
    mov al, [status]
    and al, 0x0F
    call hexdigit
    mov si, msg_crlf
    call puts
halt:
    cli
    hlt
    jmp halt

hexdigit:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 7
    jmp putc

; ES:DI = a sector read from the image, AX = the LBA it should carry.
; ZF set when it starts with 'LBA:' and that LBA.
check_tag:
    cmp dword [es:di], 'LBA:'
    jne .r
    cmp [es:di + 4], ax
.r:
    ret

puts:
    lodsb
    test al, al
    jz .ret
    call putc
    jmp puts
.ret:
    ret

putc:
    push dx
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    pop dx
    ret

drive     db 0
step      db 0
status    db 0
msg_entry db 'OS-ENTRY x86 floppy ', 0
msg_ok    db 'floppy=ok', 13, 10, 0
msg_bad   db 'floppy=bad:', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
