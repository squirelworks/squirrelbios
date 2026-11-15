; SquirrelBIOS x86 — console: VGA text and COM1
; Original code. Included by x86/squirrel-bios.asm.

; In: DS:SI = zero-terminated string, ES:DI = VGA cell, AH = attribute.
; Out: DI advanced past the string.
con_print:
    lodsb
    test al, al
    jz .done
    call con_putc
    jmp con_print
.done:
    ret

; In: AL = byte, ES:DI = VGA cell, AH = attribute.  Prints two hex digits.
con_hex8:
    push cx
    mov cl, al
    shr al, 4
    call .nibble
    mov al, cl
    and al, 0x0F
    call .nibble
    pop cx
    ret
.nibble:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 'A' - '9' - 1
.out:
    jmp con_putc

; Move DI to the start of the next VGA row, CR LF on COM1.
con_newline:
    push ax
    push cx
    push dx
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    mov ax, di
    xor dx, dx
    mov cx, VGA_LINE
    div cx
    inc ax
    mul cx
    mov di, ax
    pop dx
    pop cx
    pop ax
    ret

; In: AL = character, AH = attribute, ES:DI = VGA cell.  Out: DI += 2.
; COM1 gets the character as is. On VGA the romanized ch, gh, ng and tlh
; digraphs are folded onto the pIqaD code points in klingon-font.inc.
con_putc:
    call serial_putc
    push bx
    push cx
    push dx
    mov cl, al
    or cl, 0x20                 ; lower case, for letters
    push ax
    mov ax, di
    xor dx, dx
    mov bx, VGA_LINE
    div bx                      ; DX = byte offset within the row
    pop ax
    cmp dx, 2                   ; never fold across rows
    jb .plain
    mov bl, [es:di-2]
    or bl, 0x20
    cmp cl, 'h'
    je .h
    cmp cl, 'g'
    jne .plain
    cmp bl, 'n'
    jne .plain
    mov byte [es:di-2], KL_NG
    jmp .done
.h:
    mov bh, KL_CH
    cmp bl, 'c'
    je .fold
    mov bh, KL_GH
    cmp bl, 'g'
    je .fold
    cmp bl, 'l'
    jne .plain
    cmp dx, 4
    jb .plain
    mov bl, [es:di-4]
    or bl, 0x20
    cmp bl, 't'
    jne .plain
    mov byte [es:di-4], KL_TLH
    sub di, 2
    mov byte [es:di], ' '
    jmp .done
.fold:
    mov [es:di-2], bh
    jmp .done
.plain:
    stosw
.done:
    pop dx
    pop cx
    pop bx
    ret

; COM1: 115200 baud, 8N1, FIFOs on, interrupts off.
serial_init:
    mov dx, COM1 + 1            ; IER
    xor al, al
    out dx, al
    mov dx, COM1 + 3            ; LCR: DLAB on
    mov al, 0x80
    out dx, al
    mov dx, COM1                ; divisor 1 = 115200
    mov al, 1
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3            ; LCR: 8N1, DLAB off
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2            ; FCR: enable and clear FIFOs
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4            ; MCR: DTR, RTS
    mov al, 0x03
    out dx, al
    ret

; In: AL = character.  Gives up after UART_TIMEOUT polls so a missing
; or wedged UART cannot hang the boot.
serial_putc:
    push ax
    push cx
    push dx
    mov ah, al
    mov cx, UART_TIMEOUT
    mov dx, COM1 + 5            ; LSR
.wait:
    in al, dx
    test al, 0x20               ; THR empty
    jnz .send
    loop .wait
.send:
    mov al, ah
    mov dx, COM1
    out dx, al
    pop dx
    pop cx
    pop ax
    ret
