; SquirrelBIOS x86 — 8042 keyboard controller set-up
; Original code. Included by x86/squirrel-bios.asm.

; Program the i8042 so the OS sees XT (set-1) make/break codes: write
; the controller command byte with XLAT and keyboard IRQ enabled, enable
; scanning, and discard anything already in the output buffer.
kbd_init:
    push ax
    push cx
    mov al, 0x60                ; write command byte
    call kbd_cmd
    mov al, 0x65                ; IRQ1 + SYS + XLAT
    call kbd_data
    mov al, 0xAE                ; enable keyboard interface
    call kbd_cmd
    call kbd_drain
    pop cx
    pop ax
    ret

kbd_drain:
    mov cx, 32
.poll:
    in al, KBD_STATUS
    test al, 1
    jz .done
    in al, KBD_DATA
    loop .poll
.done:
    ret

; Wait for the input buffer to empty, then write AL to the command port.
kbd_cmd:
    call kbd_wait_in
    out KBD_STATUS, al
    ret

; Wait for the input buffer to empty, then write AL to the data port.
kbd_data:
    call kbd_wait_in
    out KBD_DATA, al
    ret

kbd_wait_in:
    push ax
    push cx
    mov cx, 0xFFFF
.wait:
    in al, KBD_STATUS
    test al, 2                  ; input buffer full
    jz .ok
    loop .wait
.ok:
    pop cx
    pop ax
    ret

; ---------------------------------------------------------------------------
; Real-mode IVT, installed before every hand-off. The ROM is mapped at
; 0xF0000, which is the segment stored in the IVT.
;
;   INT 10h AH=0x0E   teletype to VGA text (the boot sector's failure
;                     line, and the kernel's old-CPU line)
;   INT 13h AH=0x00   reset the primary master
;   INT 13h AH=0x02   CHS read
;   INT 13h AH=0x03   CHS write
;   INT 13h AH=0x08   drive parameters (16 heads, 63 sectors)
;   INT 13h AH=0x42   extended read (LBA), packet size 0x10
;   INT 13h AH=0x43   extended write (LBA), packet size 0x10
;   INT 15h EAX=E820  next SQMM entry, same table as 0x0500
;   INT 16h AH=0x00   read keystroke (wait)
;   INT 16h AH=0x01   check keystroke (ZF clear if ready)
;   INT 1Ah AH=0x00   read tick count since midnight
;   INT 1Ah AH=0x02   read RTC time (BCD)
;
; Any other request on those vectors returns with CF set (INT 16h check
; clears CF and uses ZF). IRQ0 increments the BDA tick count; IRQ1 feeds
; the keyboard buffer. Every other vector just irets.
; ---------------------------------------------------------------------------
