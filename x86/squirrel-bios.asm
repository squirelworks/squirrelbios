; SquirrelBIOS — x86 firmware ROM for QEMU PC
; 64 KB image. Reset vector at offset 0xFFF0.
; Original code.
;
; Build:  nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
; Run:    qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide
;
; Behaviour:
;   - Set segments and stack
;   - Program COM1 (115200 8N1); every message goes to VGA text and COM1
;   - Build a memory map at 0x0000:0x0500 (memmap.inc)
;   - Print banner
;   - ATA PIO read of LBA 0 (sector 1) into 0x7C00, with timeouts,
;     status checks and retries (soft reset between attempts)
;   - On failure: print the reason and the ATA status/error registers, halt
;   - On success: jump to 0x0000:0x7C00 with DL = 0x80, DI = 0x0500 (memory map)

BITS 16

; Primary ATA channel, legacy ports
ATA_DATA        equ 0x1F0
ATA_ERROR       equ 0x1F1
ATA_COUNT       equ 0x1F2
ATA_LBA_LO      equ 0x1F3
ATA_LBA_MID     equ 0x1F4
ATA_LBA_HI      equ 0x1F5
ATA_DRIVE       equ 0x1F6
ATA_STATUS      equ 0x1F7       ; read: status, write: command
ATA_CTRL        equ 0x3F6       ; read: alt status, write: device control

ST_BSY          equ 0x80
ST_DRDY         equ 0x40
ST_DF           equ 0x20
ST_DRQ          equ 0x08
ST_ERR          equ 0x01

CTRL_NIEN       equ 0x02        ; no interrupts, we poll
CTRL_SRST       equ 0x04

CMD_READ_SECTORS equ 0x20

ATA_TIMEOUT     equ 0x00400000  ; status polls before giving up
ATA_ATTEMPTS    equ 3

COM1            equ 0x3F8
UART_TIMEOUT    equ 0xFFFF      ; THR-empty polls per character

VGA_SEG         equ 0xB800
VGA_LINE        equ 160         ; bytes per 80-column text row
ATTR_NORMAL     equ 0x0F
ATTR_ERROR      equ 0x0C

entry:
    cli
    cld
    mov ax, cs                  ; strings live in the ROM segment
    mov ds, ax
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00
    mov ax, VGA_SEG
    mov es, ax

    call build_memmap
    call serial_init

    ; Banner
    xor di, di
    mov si, banner
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

    ; Disk: up to ATA_ATTEMPTS reads of LBA 0, soft reset between them
    mov bp, ATA_ATTEMPTS
.try:
    call ata_read_lba0
    jnc handoff
    cmp si, msg_nodrive         ; nothing to retry against
    je disk_fail
    cmp si, msg_atapi
    je disk_fail
    dec bp
    jz disk_fail
    call ata_reset
    jmp .try

handoff:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov dl, 0x80                ; boot drive
    mov di, MEMMAP_ADDR         ; memory map header
    jmp 0x0000:0x7C00

; In: SI = reason, BL = ATA status, BH = ATA error register
disk_fail:
    mov ah, ATTR_ERROR
    call con_print
    mov si, msg_status
    call con_print
    mov al, bl
    call con_hex8
    mov si, msg_error
    call con_print
    mov al, bh
    call con_hex8
    call con_newline
    mov si, msg_halted
    call con_print
    call con_newline
.halt:
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; ATA PIO
; ---------------------------------------------------------------------------

; Read LBA 0 of the primary master into 0x0000:0x7C00.
; Out: CF = 0 on success.
;      CF = 1 on failure, SI = reason, BL = status, BH = error register.
ata_read_lba0:
    ; 0xFF is a floating bus: no controller on the primary channel
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive

    mov dx, ATA_DRIVE
    mov al, 0xE0                ; master, LBA, LBA[27:24] = 0
    out dx, al
    call ata_delay400

    ; 0x00 or 0xFF after selecting: nothing answered as master,
    ; unless an ATAPI device left its signature in the LBA registers
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive
    test al, al
    jnz .present
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0x14
    jne .nodrive
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xEB
    je .atapi
    jmp .nodrive
.present:

    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov dx, ATA_COUNT
    mov al, 1
    out dx, al
    xor al, al                  ; LBA 0
    mov dx, ATA_LBA_LO
    out dx, al
    mov dx, ATA_LBA_MID
    out dx, al
    mov dx, ATA_LBA_HI
    out dx, al

    mov dx, ATA_STATUS
    mov al, CMD_READ_SECTORS
    out dx, al
    call ata_delay400

    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    push es
    xor ax, ax
    mov es, ax
    mov di, 0x7C00
    mov cx, 256
    mov dx, ATA_DATA
    rep insw
    pop es

    ; Command must finish cleanly, otherwise the data is suspect
    call ata_delay400
    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error
    test al, ST_DRQ             ; drive still has data: count mismatch
    jnz .error
    clc
    ret

.nodrive:
    mov si, msg_nodrive
    jmp .failed
.atapi:
    mov si, msg_atapi
    jmp .failed
.timeout:
    mov si, msg_timeout
    jmp .failed
.error:
    mov si, msg_readerr
.failed:
    mov dx, ATA_STATUS
    in al, dx
    mov bl, al
    mov dx, ATA_ERROR
    in al, dx
    mov bh, al
    stc
    ret

; Poll status until BSY is clear and any bit in BL is set.
; In: BL = mask.  Out: AL = status, CF = 1 on timeout.
ata_poll:
    mov ecx, ATA_TIMEOUT
    mov dx, ATA_STATUS
.poll:
    in al, dx
    test al, ST_BSY
    jnz .next
    test al, bl
    jnz .done
.next:
    dec ecx
    jnz .poll
    stc
    ret
.done:
    clc
    ret

; Software reset of the primary channel, then wait for BSY to clear.
ata_reset:
    mov dx, ATA_CTRL
    mov al, CTRL_SRST | CTRL_NIEN
    out dx, al
    call ata_delay400           ; SRST must be held >= 5 us
    call ata_delay400
    call ata_delay400
    mov al, CTRL_NIEN
    out dx, al
    mov bl, 0xFF & ~ST_BSY      ; any status once BSY drops
    call ata_poll
    ret

; ~400 ns: four reads of the alternate status register.
ata_delay400:
    push ax
    push dx
    mov dx, ATA_CTRL
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop dx
    pop ax
    ret

; ---------------------------------------------------------------------------
; Console: VGA text buffer and COM1 together
; ---------------------------------------------------------------------------

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
con_putc:
    stosw
    jmp serial_putc

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

%include "memmap.inc"

banner      db 'SquirrelBIOS x86', 0
msg_nodrive db 'disk: no drive on primary master', 0
msg_atapi   db 'disk: primary master is ATAPI (CD/DVD), not supported', 0
msg_timeout db 'disk: timeout waiting for drive', 0
msg_readerr db 'disk: read of LBA 0 failed', 0
msg_status  db '  status=', 0
msg_error   db ' error=', 0
msg_halted  db 'boot halted', 0

; Pad so the reset vector lands at offset 0xFFF0
times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

times 65536 - ($ - $$) db 0
