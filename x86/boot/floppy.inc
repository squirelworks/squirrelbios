; SquirrelBIOS x86 — boot from diskette drive A:
; Original code. Included by x86/squirrel-bios.asm when CONFIG_FLOPPY.

; Equipment word: bit 0 = diskette drives present, bits 7-6 = count - 1.
; Called after bda_init. Preserves every register.
fdc_bda_init:
    push ax
    call fdc_drives
    jz .done
    dec al
    shl al, 6
    or al, 1
    or [ss:BDA_EQUIP], al
.done:
    pop ax
    ret

; If CMOS reports drive A:, read its boot sector (C0 H0 S1) to 0x7C00.
; A signed sector is entered with DL = 0x00 (does not return). No disk
; in the drive returns quietly; a disk without 0xAA55 says so first.
; Either way the caller goes on to the hard disk. Preserves ES and DI
; (the console position) and DS.
floppy_boot:
    mov al, CMOS_FLOPPY
    call cmos_read
    test al, 0xF0
    jz .ret
    push ds
    push es
    push di
    xor ax, ax
    mov ds, ax
    mov es, ax
    call fdc_reset
    jc .quiet
    mov si, 2                   ; the first read after reset may see a media change
.try:
    xor bl, bl                  ; drive A:
    xor ch, ch
    xor dh, dh
    mov cl, 1
    mov di, BOOT_SECTOR
    xor ah, ah
    call fdc_xfer
    jnc .read
    call fdc_reset
    dec si
    jnz .try
    jmp .quiet
.read:
    cmp word [BOOT_SECTOR + 0x1FE], 0xAA55
    jne .nosig
    pop di
    pop es
    pop ds
    mov byte [ss:BOOT_DL], 0x00
    mov si, msg_fdboot
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    jmp handoff
.nosig:
    call fdc_motor_off
    pop di
    pop es
    pop ds
    mov si, msg_fdnosig
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    ret
.quiet:
    call fdc_motor_off
    pop di
    pop es
    pop ds
.ret:
    ret

; All motors off, controller out of reset. Preserves every register.
fdc_motor_off:
    push ax
    push dx
    mov dx, FDC_DOR
    mov al, FDC_DOR_RESET_N | FDC_DOR_DMA
    out dx, al
    mov byte [BDA_FD_MOTOR], 0
    pop dx
    pop ax
    ret
