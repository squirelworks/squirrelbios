; SquirrelBIOS x86 — INT 10h text video services
; Original code. Included by x86/squirrel-bios.asm.

; Stacked real-mode frame: [bp+2] = IP, [bp+4] = CS, [bp+6] = FLAGS.
; AH=0x02 set cursor, 0x03 get cursor, 0x06/0x07 scroll, 0x0E teletype,
; 0x0F get mode. Page BH is ignored (only page 0). Other AH -> CF set.
int10_handler:
    push bp
    mov bp, sp
    cmp ah, 0x0E
    je .tele
    cmp ah, 0x02
    je .setcur
    cmp ah, 0x03
    je .getcur
    cmp ah, 0x06
    je .scroll
    cmp ah, 0x07
    je .scroll
    cmp ah, 0x0F
    je .getmode
    or word [bp + 6], 1
    pop bp
    iret
.tele:
    call tty_putc
    jmp .ok
.setcur:
    call vid_set_cursor
    jmp .ok
.getcur:
    call vid_get_cursor
    jmp .ok
.getmode:
    mov ah, VGA_COLS
    mov al, 3
    xor bh, bh
    jmp .ok
.scroll:
    call vid_scroll
.ok:
    and word [bp + 6], 0xFFFE
    pop bp
    iret

; DH=row, DL=col. Clamps to 80x25, stores BDA_CURSOR, updates CRTC.
vid_set_cursor:
    push ds
    push ax
    push bx
    xor ax, ax
    mov ds, ax
    mov bl, dl
    cmp bl, VGA_COLS
    jb .c
    mov bl, VGA_COLS - 1
.c:
    mov bh, dh
    cmp bh, VGA_ROWS
    jb .r
    mov bh, VGA_ROWS - 1
.r:
    mov [BDA_CURSOR], bx
    call tty_hwcursor
    pop bx
    pop ax
    pop ds
    ret

; Out: DH=row, DL=col, CH/CL = cursor type, BH = 0.
vid_get_cursor:
    push ds
    push ax
    xor ax, ax
    mov ds, ax
    mov ax, [BDA_CURSOR]
    mov dl, al
    mov dh, ah
    mov cx, 0x0607
    xor bh, bh
    pop ax
    pop ds
    ret

; AH=06 scroll up / 07 scroll down. AL = line count (0 = blank window).
; BH = attribute for blank rows. CH/CL = upper-left row/col,
; DH/DL = lower-right row/col.
;
; Full-screen one-line scrolls use the fast path; any other request
; blanks the window (AL=0 behaviour). Locals live on the stack.
vid_scroll:
    push bp
    mov bp, sp
    sub sp, 8
    ; [bp-1]=fn AH, [bp-2]=lines AL, [bp-3]=attr BH,
    ; [bp-4]=r0 CH, [bp-5]=c0 CL, [bp-6]=r1 DH, [bp-7]=c1 DL
    mov [bp - 1], ah
    mov [bp - 2], al
    mov [bp - 3], bh
    mov [bp - 4], ch
    mov [bp - 5], cl
    mov [bp - 6], dh
    mov [bp - 7], dl
    push ds
    push es
    pusha
    mov ax, VGA_SEG
    mov es, ax
    cmp byte [bp - 5], VGA_COLS
    jb .ok0
    mov byte [bp - 5], VGA_COLS - 1
.ok0:
    cmp byte [bp - 7], VGA_COLS
    jb .ok1
    mov byte [bp - 7], VGA_COLS - 1
.ok1:
    cmp byte [bp - 4], VGA_ROWS
    jb .ok2
    mov byte [bp - 4], VGA_ROWS - 1
.ok2:
    cmp byte [bp - 6], VGA_ROWS
    jb .ok3
    mov byte [bp - 6], VGA_ROWS - 1
.ok3:
    mov al, [bp - 4]
    cmp al, [bp - 6]
    ja .done
    mov al, [bp - 5]
    cmp al, [bp - 7]
    ja .done
    cmp byte [bp - 2], 1
    jne .blank
    cmp byte [bp - 4], 0
    jne .blank
    cmp byte [bp - 5], 0
    jne .blank
    cmp byte [bp - 6], VGA_ROWS - 1
    jne .blank
    cmp byte [bp - 7], VGA_COLS - 1
    jne .blank
    cmp byte [bp - 1], 0x06
    je .up
    mov ax, VGA_SEG
    mov ds, ax
    std
    mov si, VGA_LINE * (VGA_ROWS - 1) - 2
    mov di, VGA_LINE * VGA_ROWS - 2
    mov cx, VGA_COLS * (VGA_ROWS - 1)
    rep movsw
    cld
    mov ah, [bp - 3]
    mov al, ' '
    xor di, di
    mov cx, VGA_COLS
    rep stosw
    jmp .done
.up:
    call tty_scroll
    mov ah, [bp - 3]
    cmp ah, ATTR_NORMAL
    je .done
    mov al, ' '
    mov di, VGA_LINE * (VGA_ROWS - 1)
    mov cx, VGA_COLS
    rep stosw
    jmp .done
.blank:
    mov ah, [bp - 3]
    mov al, ' '
    mov bl, [bp - 4]
.brow:
    cmp bl, [bp - 6]
    ja .done
    movzx di, bl
    imul di, di, VGA_LINE
    movzx cx, byte [bp - 5]
    add di, cx
    add di, cx
    movzx cx, byte [bp - 7]
    movzx dx, byte [bp - 5]
    sub cx, dx
    inc cx
    push ax
    rep stosw
    pop ax
    inc bl
    jmp .brow
.done:
    popa
    pop es
    pop ds
    mov sp, bp
    pop bp
    ret

; Teletype. AL = character. CR, LF, BS and BEL are the usual ones;
; everything else is written in the firmware's normal attribute and
; the cursor advances, wrapping and scrolling at the bottom.
tty_putc:
    pushad
    push ds
    push es
    xor dx, dx
    mov ds, dx
    mov bx, [BDA_CURSOR]        ; BL = column, BH = row
    cmp al, 0x0D
    je .cr
    cmp al, 0x0A
    je .lf
    cmp al, 0x08
    je .bs
    cmp al, 0x07
    je .done
    movzx di, bh
    imul di, di, VGA_LINE
    movzx cx, bl
    shl cx, 1
    add di, cx
    mov ah, ATTR_NORMAL
    mov cx, VGA_SEG
    mov es, cx
    mov [es:di], ax
    inc bl
    cmp bl, VGA_COLS
    jb .store
    xor bl, bl
    jmp .lf
.cr:
    xor bl, bl
    jmp .store
.bs:
    test bl, bl
    jz .store
    dec bl
    jmp .store
.lf:
    inc bh
    cmp bh, VGA_ROWS
    jb .store
    call tty_scroll
    mov bh, VGA_ROWS - 1
.store:
    mov [BDA_CURSOR], bx
    call tty_hwcursor
.done:
    pop es
    pop ds
    popad
    ret

tty_scroll:
    push ds
    push es
    push ax
    push cx
    push si
    push di
    mov ax, VGA_SEG
    mov ds, ax
    mov es, ax
    mov si, VGA_LINE
    xor di, di
    mov cx, VGA_COLS * (VGA_ROWS - 1)
    cld
    rep movsw
    mov ax, (ATTR_NORMAL << 8) | ' '
    mov cx, VGA_COLS
    rep stosw
    pop di
    pop si
    pop cx
    pop ax
    pop es
    pop ds
    ret

; BL = column, BH = row. Programs the CRTC cursor.
tty_hwcursor:
    push ax
    push cx
    push dx
    movzx ax, bh
    mov cx, VGA_COLS
    mul cx
    movzx cx, bl
    add ax, cx
    mov cx, ax
    mov dx, VGA_CRTC_I
    mov al, 0x0E
    out dx, al
    inc dx
    mov al, ch
    out dx, al
    dec dx
    mov al, 0x0F
    out dx, al
    inc dx
    mov al, cl
    out dx, al
    pop dx
    pop cx
    pop ax
    ret
