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
;   - Put the VGA into 80x25 text mode with the pIqaD-style font from
;     klingon-font.inc (joke klingon-font branch); COM1 stays plain ASCII
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
ATTR_KLINGON    equ 0x0A

PCI_ADDR        equ 0xCF8
PCI_DATA        equ 0xCFC

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
    call vga_init

    ; Banner
    xor di, di
    mov si, banner
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    mov si, banner_tlh
    mov ah, ATTR_KLINGON
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

; In: AL = character, AH = attribute, ES:DI = VGA cell.  Out: DI advanced.
; COM1 gets the character as is. On VGA the romanized ch, gh, ng and tlh
; fold into one pIqaD glyph by rewriting the cells just written, so
; DI moves back instead of forward for those.
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

; ---------------------------------------------------------------------------
; VGA: 80x25 text mode from scratch (no VGA BIOS runs), Klingon font
; ---------------------------------------------------------------------------

vga_init:
    pushad
    push es
    call vga_pci_enable

    mov dx, 0x3C4               ; sequencer: hold in reset while
    mov ax, 0x0100              ; the clock changes
    out dx, ax
    mov dx, 0x3C2               ; misc output: colour, 28 MHz, 400 lines
    mov al, 0x67
    out dx, al
    mov si, vga_seq
    mov dx, 0x3C4
    mov cx, 5
    call vga_write_indexed

    mov dx, 0x3D4               ; CRTC: unprotect registers 0-7 first
    mov ax, 0x0E11
    out dx, ax
    mov si, vga_crtc
    mov cx, 25
    call vga_write_indexed

    mov si, vga_gc
    mov dx, 0x3CE
    mov cx, 9
    call vga_write_indexed

    mov dx, 0x3DA               ; attribute controller: reset flip-flop
    in al, dx
    mov dx, 0x3C0
    mov si, vga_ac
    xor bl, bl
.ac:
    mov al, bl
    out dx, al
    lodsb
    out dx, al
    inc bl
    cmp bl, 21
    jb .ac
    mov al, 0x20                ; palette done, video on
    out dx, al

    mov dx, 0x3C8               ; DAC: the 16 text colours
    xor al, al
    out dx, al
    inc dx
    mov si, vga_dac
    mov cx, 16 * 3
.dac:
    lodsb
    out dx, al
    loop .dac

    ; Font: plane 2, 32 bytes per glyph, 16 used
    mov dx, 0x3C4
    mov ax, 0x0402              ; write plane 2 only
    out dx, ax
    mov ax, 0x0604              ; sequential addressing
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0204              ; read plane 2
    out dx, ax
    mov ax, 0x0005              ; no odd/even
    out dx, ax
    mov ax, 0x0006              ; A000, 64 KB
    out dx, ax
    mov ax, 0xA000
    mov es, ax
    mov si, klingon_font
    xor di, di
    mov bx, 256
.glyph:
    mov cx, 8
    rep movsw
    add di, 16
    dec bx
    jnz .glyph
    mov dx, 0x3C4               ; back to text: planes 0 and 1, odd/even
    mov ax, 0x0302
    out dx, ax
    mov ax, 0x0204
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0004
    out dx, ax
    mov ax, 0x1005
    out dx, ax
    mov ax, 0x0E06
    out dx, ax

    mov ax, VGA_SEG             ; clear the screen
    mov es, ax
    xor di, di
    mov ax, 0x0720
    mov cx, 80 * 25
    rep stosw
    pop es
    popad
    ret

; In: DX = index port, DS:SI = values for indices 0.., CX = count.
vga_write_indexed:
    xor bl, bl
.next:
    mov al, bl
    mov ah, [si]
    out dx, ax
    inc si
    inc bl
    loop .next
    ret

; The VGA's legacy ports and memory only decode once its PCI command
; register allows it. Find the display controller on bus 0 and turn
; on I/O and memory space.
vga_pci_enable:
    mov ebx, 0x80000000         ; bus 0, device 0, function 0
.dev:
    mov eax, ebx
    or al, 0x08                 ; class code
    mov dx, PCI_ADDR
    out dx, eax
    mov dx, PCI_DATA
    in eax, dx
    shr eax, 16
    cmp ax, 0x0300              ; VGA-compatible display
    je .found
    add ebx, 0x800              ; next device
    cmp ebx, 0x80010000
    jb .dev
    ret
.found:
    mov eax, ebx
    or al, 0x04                 ; command
    mov dx, PCI_ADDR
    out dx, eax
    mov dx, PCI_DATA
    in ax, dx
    or ax, 0x0003
    out dx, ax
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

%include "memmap.inc"
%include "klingon-font.inc"

; Standard 80x25 text mode register values
vga_seq     db 0x03, 0x00, 0x03, 0x00, 0x02
vga_crtc    db 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F
            db 0x00, 0x4F, 0x2D, 0x0E, 0x00, 0x00, 0x00, 0x00   ; cursor off
            db 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
vga_gc      db 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
vga_ac      db 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
            db 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
            db 0x0C, 0x00, 0x0F, 0x08, 0x00
vga_dac     db 0x00,0x00,0x00, 0x00,0x00,0x2A, 0x00,0x2A,0x00, 0x00,0x2A,0x2A
            db 0x2A,0x00,0x00, 0x2A,0x00,0x2A, 0x2A,0x15,0x00, 0x2A,0x2A,0x2A
            db 0x15,0x15,0x15, 0x15,0x15,0x3F, 0x15,0x3F,0x15, 0x15,0x3F,0x3F
            db 0x3F,0x15,0x15, 0x3F,0x15,0x3F, 0x3F,0x3F,0x15, 0x3F,0x3F,0x3F

banner      db 'SquirrelBIOS x86', 0
banner_tlh  db "tlhIngan Hol yIjatlh! Qapla'", 0
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
