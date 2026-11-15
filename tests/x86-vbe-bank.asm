; Smoke-test: VBE banked windows and the palette (INT 10h AH=4Fh).
; Original code.
;
; 4F01h for 0101h (640x480x8): windows available, window A 64 KB at
; A000h in 64 KB steps, packed pixel, a window function. 4F02h 0101h
; without bit 14 (banked). 4F08h reports a 6-bit
; DAC. 4F09h sets palette entries 1 (red) and 2 (blue) and reads them
; back. Then the screen is filled with colour 2 a window at a time
; through 4F05h, and a colour-1 square drawn at (10,10)-(109,109) and a
; line on row 300 (past the first 64 KB) through the far window function;
; 4F05h BH=1 reads the window position back. tests/x86-vbe-run.sh
; checks the screen QEMU shows. Prints vbe=ok, or vbe=bad:<step>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
MINFO   equ 0x1400
PALBACK equ 0x1700

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov bp, 'm'
    mov ax, 0x4F01
    mov cx, 0x0101
    mov di, MINFO
    int 0x10
    cmp ax, 0x004F
    jne bad
    test byte [MINFO], 0x40             ; windowed mode available
    jnz bad
    cmp byte [MINFO + 2], 0x07          ; window A: exists, read, write
    jne bad
    cmp dword [MINFO + 4], 64 << 16 | 64 ; granularity, size: KB
    jne bad
    cmp word [MINFO + 8], 0xA000
    jne bad
    cmp byte [MINFO + 25], 8
    jne bad
    cmp byte [MINFO + 27], 4            ; packed pixel
    jne bad
    mov eax, [MINFO + 12]               ; WinFuncPtr
    test eax, eax
    jz bad
    mov [winfunc], eax

    mov bp, 's'
    mov ax, 0x4F02
    mov bx, 0x0101
    int 0x10
    cmp ax, 0x004F
    jne bad

    mov bp, 'd'
    mov ax, 0x4F08
    mov bl, 1
    int 0x10
    cmp ax, 0x004F
    jne bad
    cmp bh, 6
    jne bad

    mov bp, 'p'
    mov ax, 0x4F09
    xor bl, bl
    mov cx, 2
    mov dx, 1
    mov di, pal
    int 0x10
    cmp ax, 0x004F
    jne bad
    mov ax, 0x4F09
    mov bl, 1
    mov cx, 2
    mov dx, 1
    mov di, PALBACK
    int 0x10
    cmp ax, 0x004F
    jne bad
    mov si, pal
    mov di, PALBACK
    mov cx, 8
    repe cmpsb
    jne bad
    mov ax, 0x4F09                      ; past entry 255
    xor bl, bl
    mov cx, 2
    mov dx, 255
    mov di, pal
    int 0x10
    cmp ax, 0x014F
    jne bad

    ; Fill: five 64 KB windows hold 640x480 bytes
    mov bp, 'f'
    xor dx, dx
.window:
    mov ax, 0x4F05
    xor bx, bx
    int 0x10
    cmp ax, 0x004F
    jne bad
    push 0xA000
    pop es
    xor di, di
    mov cx, 0x8000
    mov ax, 0x0202
    rep stosw
    inc dx
    cmp dx, 5
    jb .window

    ; The square and the line through the window function
    mov bp, 'w'
    mov edx, 10                         ; row
.row:
    mov ecx, 10                         ; column
.px:
    call plot
    inc ecx
    cmp ecx, 110
    jb .px
    inc edx
    cmp edx, 110
    jb .row
    mov edx, 300
    xor ecx, ecx
.line:
    call plot
    inc ecx
    cmp ecx, 640
    jb .line

    mov bp, 'g'
    mov ax, 0x4F05
    mov bh, 1
    xor bl, bl
    int 0x10
    cmp ax, 0x004F
    jne bad
    cmp dx, (300 * 640 + 639) >> 16
    jne bad

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

; Colour 1 at column ECX, row EDX, moving the window only when needed.
; Keeps ECX and EDX.
plot:
    pushad
    imul eax, edx, 640
    add eax, ecx
    mov edi, eax
    shr eax, 16
    cmp ax, [cur_window]
    je .put
    mov [cur_window], ax
    mov dx, ax
    xor bx, bx
    call far [winfunc]
    cmp ax, 0x004F
    jne .fail
.put:
    push 0xA000
    pop es
    mov byte [es:di], 1
    xor ax, ax
    mov es, ax
    popad
    ret
.fail:
    popad
    add sp, 2
    jmp bad

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

winfunc     dd 0
cur_window  dw 0xFFFF
msg_ok    db 'vbe=ok', 13, 10, 0
msg_bad   db 'vbe=bad:', 0
msg_crlf  db 13, 10, 0

; Entries 1 and 2: blue, green, red, 0
pal       db 0, 0, 63, 0, 63, 0, 0, 0

times 510 - ($ - $$) db 0
dw 0xAA55
