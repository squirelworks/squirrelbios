; SquirrelBIOS x86 — IRQ1 and INT 16h keyboard services
; Original code. Included by x86/squirrel-bios.asm.

; IRQ1: keyboard. Read the scancode, ignore break codes, enqueue a
; make-code pair (ASCII, scancode) in the BDA ring buffer when there is
; room. EOI to the master PIC.
irq1_handler:
    push ax
    push bx
    push ds
    xor ax, ax
    mov ds, ax
    in al, KBD_DATA
    test al, 0x80
    jnz .eoi                     ; break code
    call kbd_lookup
    jc .eoi                      ; unknown make code: drop
    ; AL = ASCII, AH = scancode. Enqueue at tail if not full.
    mov bx, [BDA_KBD_TAIL]
    mov [bx], ax
    add bx, 2
    cmp bx, BDA_KBD_END
    jb .wrap
    mov bx, BDA_KBD_BUF
.wrap:
    cmp bx, [BDA_KBD_HEAD]
    je .eoi                      ; full: drop the key
    mov [BDA_KBD_TAIL], bx
.eoi:
    mov al, 0x20
    out PIC1_CMD, al
    pop ds
    pop bx
    pop ax
    iret

; In: AL = make scancode. Out: CF clear, AL = ASCII, AH = scancode;
; CF set if this firmware has no ASCII for it. Uses CS-relative table.
kbd_lookup:
    push bx
    push ds
    mov ah, al                  ; keep scancode
    cmp al, kbd_map_len
    jae .bad
    mov bx, cs
    mov ds, bx
    mov bx, kbd_map
    xlat                        ; AL = map[scancode]
    test al, al
    jz .bad
    ; AL = ASCII, AH = scancode
    pop ds
    pop bx
    clc
    ret
.bad:
    pop ds
    pop bx
    stc
    ret

; Set-1 make codes -> ASCII. Index is the scancode; 0 means "no glyph".
; Only the keys the stand-in injects need to work; a few letters and
; digits are enough for a PC-compatible INT 16h surface.
kbd_map:
    db 0, 0, '1', '2', '3', '4', '5', '6'      ; 00-07
    db '7', '8', '9', '0', '-', '=', 8, 9      ; 08-0F  BS TAB
    db 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i'  ; 10-17
    db 'o', 'p', '[', ']', 13, 0, 'a', 's'     ; 18-1F  Enter
    db 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';'  ; 20-27
    db "'", '`', 0, '\', 'z', 'x', 'c', 'v'    ; 28-2F
    db 'b', 'n', 'm', ',', '.', '/', 0, '*'    ; 30-37
    db 0, ' '                                   ; 38-39
kbd_map_len equ $ - kbd_map

; INT 16h: AH=0x00 read (wait), AH=0x01 check (ZF). Other AH -> CF set.
; Stacked frame: [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS.
int16_handler:
    push bp
    mov bp, sp
    cmp ah, 0x00
    je .read
    cmp ah, 0x01
    je .check
    or word [bp + 6], 1         ; CF
    pop bp
    iret
.read:
    push ds
    push bx
    xor bx, bx
    mov ds, bx
.wait:
    mov bx, [BDA_KBD_HEAD]
    cmp bx, [BDA_KBD_TAIL]
    jne .got
    sti
    hlt
    cli
    jmp .wait
.got:
    mov ax, [bx]
    add bx, 2
    cmp bx, BDA_KBD_END
    jb .rh
    mov bx, BDA_KBD_BUF
.rh:
    mov [BDA_KBD_HEAD], bx
    pop bx
    pop ds
    and word [bp + 6], 0xFFFE   ; clear CF
    ; ZF not meaningful for AH=00
    pop bp
    iret
.check:
    push ds
    push bx
    xor bx, bx
    mov ds, bx
    mov bx, [BDA_KBD_HEAD]
    cmp bx, [BDA_KBD_TAIL]
    je .empty
    mov ax, [bx]                ; peek; do not advance head
    pop bx
    pop ds
    and word [bp + 6], 0xFFBE   ; clear CF and ZF
    pop bp
    iret
.empty:
    pop bx
    pop ds
    and word [bp + 6], 0xFFFE   ; clear CF
    or word [bp + 6], 0x0040    ; set ZF
    pop bp
    iret

; --- end keyboard --------------------------------------------------------
