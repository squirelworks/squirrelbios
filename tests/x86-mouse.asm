; Smoke-test: INT 15h AH=C2h, the pointing device interface, served by a
; USB mouse. Original code.
;
; Checks, before any motion: C200h enable is refused (AH=5) while no
; handler is set; C201h reset answers BX=00AAh; C204h device type is 0;
; C205h with packet size 9 is refused (AH=2), with 3 accepted; C206h
; status reports disabled, resolution 2 and 100 reports a second; C207h
; sets the handler; C200h enables. Prints the OS-ENTRY line, then the
; harness moves the mouse 10 right and 5 down with the left button held.
; The handler adds up what it is given: the status must have bit 3 set
; and the left button, X must add up to +10 and Y to -5 (PS/2 counts up
; as positive), each with its sign bit in the status. Prints mouse=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
BDA_TICKS       equ 0x046C
MAX_TICKS       equ 60

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Enable without a handler: refused
    mov ax, 0xC200
    mov bh, 1
    int 0x15
    jnc .bad
    cmp ah, 5
    jne .bad
    ; Reset
    mov ax, 0xC201
    int 0x15
    jc .bad
    cmp bx, 0x00AA
    jne .bad
    ; Device type
    mov ax, 0xC204
    mov bh, 0xFF
    int 0x15
    jc .bad
    test bh, bh
    jnz .bad
    ; Initialize: 9-byte packets refused, 3 accepted
    mov ax, 0xC205
    mov bh, 9
    int 0x15
    jnc .bad
    cmp ah, 2
    jne .bad
    mov ax, 0xC205
    mov bh, 3
    int 0x15
    jc .bad
    ; Status: disabled, resolution 2, 100 a second
    mov ax, 0xC206
    xor bh, bh
    int 0x15
    jc .bad
    test bl, 0x20
    jnz .bad
    cmp cl, 2
    jne .bad
    cmp dl, 100
    jne .bad
    ; Handler, then enable
    mov ax, 0xC207
    mov bx, handler
    int 0x15
    jc .bad
    mov ax, 0xC200
    mov bh, 1
    int 0x15
    jc .bad

    mov si, msg_entry
    call puts

    mov ebx, [BDA_TICKS]
    sti
.wait:
    cmp word [sum_x], 10
    jne .more
    cmp word [sum_y], -5
    jne .more
    test byte [seen], 1         ; the left button
    jnz .done_ok
.more:
    mov eax, [BDA_TICKS]
    sub eax, ebx
    cmp eax, MAX_TICKS
    jae .bad
    hlt
    jmp .wait

.done_ok:
    cli
    cmp byte [bad_status], 0
    jne .bad
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

; The mouse handler: [bp+12] status, [bp+10] X, [bp+8] Y, [bp+6] 0
handler:
    push bp
    mov bp, sp
    push ax
    push ds
    xor ax, ax
    mov ds, ax
    mov ax, [bp + 12]
    or [seen], al
    test al, 0x08
    jnz .bit3
    mov byte [bad_status], 1
.bit3:
    ; X: 8 bits of data, the sign in status bit 4
    mov ah, 0
    test byte [bp + 12], 0x10
    jz .xpos
    mov ah, 0xFF
.xpos:
    mov al, [bp + 10]
    add [sum_x], ax
    mov ah, 0
    test byte [bp + 12], 0x20
    jz .ypos
    mov ah, 0xFF
.ypos:
    mov al, [bp + 8]
    add [sum_y], ax
    pop ds
    pop ax
    pop bp
    retf

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

sum_x       dw 0
sum_y       dw 0
seen        db 0
bad_status  db 0
msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'mouse=ok', 0
msg_bad   db 'mouse=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
