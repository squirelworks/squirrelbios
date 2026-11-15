; Smoke-test: the VBE protected-mode interface (INT 10h AX=4F0Ah).
; Original code.
;
; 4F02h 0101h (640x480x8, banked), then 4F0Ah BL=00h: AX = 004Fh, a table
; of CX bytes whose four offsets fall inside it and whose port list
; starts with 01CEh. The table is copied to 8000h and its code called
; from 32-bit protected mode (flat segments): Set Window to 2, Set
; Display Start to byte 1928 / 4 (pixel 8 of line 3), Set Primary
; Palette entry 5 to red 10, green 20, blue 30. Back in real mode, 4F05h
; BH=01h reads window 2, 4F07h BL=01h reads (8, 3), and the DAC reads
; back entry 5. Prints pmi=ok, or pmi=bad:<step>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
COPY    equ 0x8000

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00

    mov byte [step], 'm'
    mov ax, 0x4F02
    mov bx, 0x0101
    int 0x10
    cmp ax, 0x004F
    jne bad

    mov byte [step], 'i'
    mov ax, 0x4F0A
    xor bx, bx
    int 0x10
    cmp ax, 0x004F
    jne bad
    ; Copy it, then check it there
    push ds
    push es
    pop ds
    mov si, di
    xor ax, ax
    mov es, ax
    mov di, COPY
    mov dx, cx
    rep movsb
    pop ds
    mov byte [step], 'c'
    mov si, COPY
    mov cx, 4
.offs:
    lodsw
    cmp ax, 8
    jb bad
    cmp ax, dx
    jae bad
    loop .offs
    mov byte [step], 'p'
    mov bx, [COPY + 6]
    cmp word [COPY + bx], 0x01CE
    jne bad
    ; The three entry points, as flat addresses
    mov di, entries
    mov si, COPY
    mov cx, 3
.entry:
    lodsw
    add ax, COPY
    stosw
    add di, 2
    loop .entry

    ; Into 32-bit protected mode, the calls, and back
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm32
BITS 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    movzx esp, sp
    xor ebx, ebx
    mov edx, 2
    call [entries]                      ; Set Window
    mov ecx, 1928 / 4
    xor edx, edx
    call [entries + 4]                  ; Set Display Start
    mov ecx, 1
    mov edx, 5
    mov edi, pal
    call [entries + 8]                  ; Set Primary Palette
    jmp 0x18:pm16
BITS 16
pm16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0:real
real:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov byte [step], 'w'
    mov ax, 0x4F05
    mov bx, 0x0100
    int 0x10
    cmp dx, 2
    jne bad
    mov byte [step], 's'
    mov ax, 0x4F07
    mov bx, 0x0001
    int 0x10
    cmp cx, 8
    jne bad
    cmp dx, 3
    jne bad
    mov byte [step], 'l'
    mov dx, 0x3C7
    mov al, 5
    out dx, al
    mov dx, 0x3C9
    in al, dx
    cmp al, 10
    jne bad
    in al, dx
    cmp al, 20
    jne bad
    in al, dx
    cmp al, 30
    jne bad

    mov si, msg_ok
    jmp done
bad:
    mov si, msg_bad
    mov al, [step]
    mov [msg_bad + 8], al
done:
    call puts
halt:
    hlt
    jmp halt

puts:
    lodsb
    test al, al
    jz .r
    mov ah, al
    mov dx, COM1 + 5
.w:
    in al, dx
    test al, 0x20
    jz .w
    mov al, ah
    mov dx, COM1
    out dx, al
    jmp puts
.r:
    ret

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 08h: flat 32-bit code
    dq 0x00CF92000000FFFF               ; 10h: flat data
    dq 0x00009A000000FFFF               ; 18h: 16-bit code, base 0
    dq 0x000092000000FFFF               ; 20h: 16-bit data, 64 KB
gdtr:
    dw gdtr - gdt - 1
    dd gdt
entries dd 0, 0, 0
pal     db 30, 20, 10, 0                ; blue, green, red
step    db 0
msg_ok  db 'pmi=ok', 13, 10, 0
msg_bad db 'pmi=bad:?', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
