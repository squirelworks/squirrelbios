; Smoke-test: El Torito no-emulation boot from an ATAPI CD.
; Original code.
;
; tests/mkiso.py puts this image at block 20 of a 64-block CD with a
; boot catalog naming it (load segment 07C0h, all of it loaded), and
; tags the blocks after it with 'LBA:' and the block number. QEMU's
; -cdrom is the secondary master. Steps:
;   d  DL = E0h, and the last dword of the image arrived ('LAST')
;   e  AH=41h BX=55AAh: CF clear, BX = AA55h, CX bit 0
;   s  AX=4B01h: 13h-byte packet, no emulation, drive E0h, controller
;      CD_CTRL (1: secondary), device CD_DEV (0: master), image block
;      20, load segment 07C0h, the image's sector count
;   r  AH=42h, 2 blocks from block 40: both tags, 2048 bytes apart
;   p  AH=48h: 2048 bytes per block, 64 blocks
;   x  AH=08h (not served for a CD): CF set
; Prints cd=ok, or cd=bad:<step> <AH in hex>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
%ifndef CD_CTRL
%define CD_CTRL 1                   ; -cdrom: secondary master
%endif
%ifndef CD_DEV
%define CD_DEV 0
%endif
SECTORS equ 4                       ; 512-byte sectors in this image
BUF     equ 0x2000                  ; segment of the read buffer

start:
    jmp 0:main                      ; entered at 07C0h:0000h

main:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    mov [drive], dl
    mov si, msg_entry
    call puts

    mov byte [step], 'd'
    xor ah, ah
    cmp byte [drive], 0xE0
    jne bad
    cmp dword [marker], 'LAST'
    jne bad

    mov byte [step], 'e'
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive]
    int 0x13
    jc bad
    cmp bx, 0xAA55
    jne bad
    test cl, 1
    jz bad

    mov byte [step], 's'
    mov si, spec
    mov ax, 0x4B01
    mov dl, [drive]
    int 0x13
    jc bad
    xor ah, ah
    cmp byte [spec], 0x13
    jne bad
    cmp byte [spec + 1], 0
    jne bad
    cmp byte [spec + 2], 0xE0
    jne bad
    cmp byte [spec + 3], CD_CTRL
    jne bad
    cmp dword [spec + 4], 20
    jne bad
    cmp word [spec + 8], CD_DEV
    jne bad
    cmp word [spec + 12], 0x07C0
    jne bad
    cmp word [spec + 14], SECTORS
    jne bad

    mov byte [step], 'r'
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc bad
    mov ax, BUF
    mov es, ax
    xor ah, ah
    cmp dword [es:0], 'LBA:'
    jne bad
    cmp dword [es:4], 40
    jne bad
    cmp dword [es:2048], 'LBA:'
    jne bad
    cmp dword [es:2048 + 4], 41
    jne bad

    mov byte [step], 'p'
    mov word [params], 0x1A
    mov si, params
    mov ah, 0x48
    mov dl, [drive]
    int 0x13
    jc bad
    cmp word [params + 24], 2048
    jne bad
    cmp dword [params + 16], 64
    jne bad

    mov byte [step], 'x'
    mov ah, 0x08
    mov dl, [drive]
    int 0x13
    jnc bad

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
    mov al, [status]
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

dap:
    db 0x10, 0
    dw 2                            ; blocks
    dw 0, BUF
    dq 40

drive     db 0
step      db 0
status    db 0
msg_entry db 'OS-ENTRY x86 cd ', 0
msg_ok    db 'cd=ok', 13, 10, 0
msg_bad   db 'cd=bad:', 0
msg_crlf  db 13, 10, 0
spec      times 0x13 db 0
params    times 0x1A db 0

times SECTORS * 512 - 4 - ($ - $$) db 0
marker    db 'LAST'
