; Smoke-test: VBE 3.0 (INT 10h AH=4Fh) and a linear frame buffer.
; Original code.
;
; 4F00h with 'VBE2': 'VESA', version 3.0, 0112h in the mode list. 4F01h
; for 0112h: linear frame buffer, 640x480, 32 bits, its address. 4F02h for 0112h without bit 14 is refused,
; with it accepted; 4F03h reports 4112h. Then, through unreal mode, the
; screen is filled with 336699h and a red square drawn at (10,10)-(109,
; 109); tests/x86-vbe-run.sh checks the screen QEMU shows. Built with
; -DTEXT, 4F02h BX=3 then goes back to text mode, 4F03h reports 3, and a
; teletype 'X' lands at B800:0000. Prints vbe=ok, or vbe=bad:<step>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
INFO    equ 0x1000
MINFO   equ 0x1400

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov bp, 'i'
    mov dword [INFO], 'VBE2'
    mov di, INFO
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne bad
    cmp dword [INFO], 'VESA'
    jne bad
    cmp word [INFO + 4], 0x0300
    jne bad
    lfs si, [INFO + 0x0E]
.list:
    mov ax, [fs:si]
    cmp ax, 0xFFFF
    je bad
    add si, 2
    cmp ax, 0x0112
    jne .list

    mov bp, 'm'
    mov ax, 0x4F01
    mov cx, 0x0112
    mov di, MINFO
    int 0x10
    cmp ax, 0x004F
    jne bad
    test byte [MINFO], 0x80
    jz bad
    cmp word [MINFO + 18], 640
    jne bad
    cmp word [MINFO + 20], 480
    jne bad
    cmp byte [MINFO + 25], 32
    jne bad
    mov eax, [MINFO + 40]
    test eax, eax
    jz bad
    mov [lfb], eax

    mov bp, 's'
    mov ax, 0x4F02
    mov bx, 0x0112
    int 0x10
    cmp ax, 0x014F
    jne bad
    mov ax, 0x4F02
    mov bx, 0x4112
    int 0x10
    cmp ax, 0x004F
    jne bad
    mov ax, 0x4F03
    int 0x10
    cmp ax, 0x004F
    jne bad
    cmp bx, 0x4112
    jne bad

    ; Unreal mode: FS with a 4 GB limit
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $ + 2
    mov bx, 8
    mov fs, bx
    and al, 0xFE
    mov cr0, eax
    xor bx, bx
    mov fs, bx
    ; Fill, then the square
    mov edi, [lfb]
    mov ecx, 640 * 480
.fill:
    mov dword [fs:edi], 0x00336699
    add edi, 4
    dec ecx
    jnz .fill
    mov edx, 10                 ; line
.row:
    mov edi, edx
    imul edi, edi, 640 * 4
    add edi, [lfb]
    add edi, 10 * 4
    mov ecx, 100
.px:
    mov dword [fs:edi], 0x00FF0000
    add edi, 4
    loop .px
    inc edx
    cmp edx, 110
    jb .row

%ifdef TEXT
    mov bp, 't'
    mov ax, 0x4F02
    mov bx, 3
    int 0x10
    cmp ax, 0x004F
    jne bad
    mov ax, 0x4F03
    int 0x10
    cmp bx, 3
    jne bad
    mov ax, 0x0E58
    xor bx, bx
    int 0x10
    push word 0xB800
    pop gs
    cmp byte [gs:0], 'X'
    jne bad
%endif

    mov si, msg_ok
    call puts
    jmp halt
bad:
    mov si, msg_bad
    call puts
    mov ax, bp
    call putc
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

gdt:
    dq 0
    dq 0x00CF92000000FFFF       ; flat data
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

lfb     dd 0
msg_ok    db 'OS-ENTRY x86 vbe=ok', 13, 10, 0
msg_bad   db 'OS-ENTRY x86 vbe=bad:', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
