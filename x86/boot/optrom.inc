; SquirrelBIOS x86 — legacy option ROM scan
; Original code. Included by x86/squirrel-bios.asm.

optrom_scan:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds
    mov ax, OPTROM_SEG_FIRST
.next:
    cmp ax, OPTROM_SEG_LAST
    jae .done
    mov es, ax
    cmp word [es:0], 0xAA55
    jne .advance
    movzx cx, byte [es:2]       ; size in 512-byte units
    test cx, cx
    jz .advance
    cmp cx, 128                 ; at most 64 KB
    ja .advance
    ; Checksum over cx*512 bytes
    push ax
    xor dl, dl
    xor bx, bx
    mov ax, cx
    shl ax, 9                   ; byte count
    mov si, ax
.sum:
    add dl, [es:bx]
    inc bx
    cmp bx, si
    jb .sum
    pop ax
    test dl, dl
    jnz .advance
    ; Far call ES:0003. Preserve our DS; the ROM may clobber AX..DX.
    push ax
    pushf
    mov bx, 3
    push es
    push bx
    mov bx, sp
    call far [ss:bx]
    add sp, 4
    popf
    pop ax
.advance:
    ; Next 2 KB boundary: size may be larger; skip by max(2KB, size*512)
    movzx bx, byte [es:2]
    test bx, bx
    jz .step2k
    cmp word [es:0], 0xAA55
    jne .step2k
    shl bx, 5                   ; size*512 / 16 = size*32 paragraphs
    cmp bx, 0x80                ; at least 2 KB = 0x80 paras
    jae .skip
.step2k:
    mov bx, 0x80
.skip:
    add ax, bx
    jmp .next
.done:
    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
