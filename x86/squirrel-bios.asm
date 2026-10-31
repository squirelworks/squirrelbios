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
;   - Check the 0xAA55 signature in the last word of the sector; halt if absent
;   - If the sector carries an SQBL header (offset 0x1F0), read the extra
;     sectors it asks for (LBA 1 on) to its load address, below 0xA0000
;   - On success: jump to 0x0000:0x7C00 with DL = 0x80, DI = 0x0500 (memory map)

BITS 16

; Firmware stack while it runs: the top of the memory map page, so a load
; anywhere outside 0x0500-0x05FF and 0x7C00-0x7DFF cannot overwrite it.
; The map ends by 0x0570 (5 entries) and the deepest call chain uses well
; under 64 bytes.
FW_STACK        equ 0x0600

; SQBL header at the end of sector 0 (x86-extra-sectors)
BOOT_SECTOR     equ 0x7C00
SQBL_HDR        equ BOOT_SECTOR + 0x1F0
SQBL_MAGIC      equ 0x4C425153  ; bytes 53 51 42 4C, 'SQBL'
SQBL_MAX        equ 128         ; extra sectors
LOAD_LIMIT      equ 0xA0000     ; first byte that is not conventional RAM

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
    mov sp, FW_STACK
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

    ; Disk: sector 0 (LBA 0) to 0x7C00
    xor cl, cl
    mov ch, 1
    mov ebp, BOOT_SECTOR
    call ata_read_retry
    jc disk_fail

; A clean read without the signature is not a boot sector. Reading it
; again would give the same bytes, so this is not retried.
    cmp word [ss:BOOT_SECTOR + 0x1FE], 0xAA55    ; SS = 0
    je .signed
    mov si, msg_nosig
    jmp boot_fail
.signed:

    ; No SQBL header: the one-sector hand-off
    cmp dword [ss:SQBL_HDR], SQBL_MAGIC
    jne handoff

    ; Extra sector count, 1-128
    movzx ecx, word [ss:SQBL_HDR + 4]
    test cx, cx
    jz .range
    cmp cx, SQBL_MAX
    ja .range
    ; EAX = segment * 16 + offset, EDX = end of the load
    movzx eax, word [ss:SQBL_HDR + 6]
    shl eax, 4
    movzx edx, word [ss:SQBL_HDR + 8]
    add eax, edx
    cmp eax, LOAD_LIMIT
    jae .range
    mov edx, ecx
    shl edx, 9
    add edx, eax
    cmp edx, LOAD_LIMIT
    ja .range
    ; [EAX, EDX) must miss the memory map page and the boot sector
    cmp eax, MEMMAP_ADDR + 0x100
    jae .past_map
    cmp edx, MEMMAP_ADDR
    ja .overlap
.past_map:
    cmp eax, BOOT_SECTOR + 0x200
    jae .load
    cmp edx, BOOT_SECTOR
    ja .overlap
.load:
    mov ebp, eax
    mov ch, cl                  ; count
    mov cl, 1                   ; from LBA 1
    call ata_read_retry
    jnc handoff
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_readerr_extra
    jmp disk_fail
.range:
    mov si, msg_range
    jmp boot_fail
.overlap:
    mov si, msg_overlap
    jmp boot_fail

handoff:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
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
    jmp boot_halt

; In: SI = reason.  A failure that is not the drive's: no ATA registers.
boot_fail:
    mov ah, ATTR_ERROR
    call con_print
; Ends the line in progress, prints 'boot halted' and halts.
boot_halt:
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

; Read with up to ATA_ATTEMPTS attempts, soft reset between them.
; In/Out: as ata_read.
ata_read_retry:
    push ax
    mov al, ATA_ATTEMPTS
.try:
    call ata_read
    jnc .done
    cmp si, msg_nodrive         ; nothing to retry against
    je .fail
    cmp si, msg_atapi
    je .fail
    dec al
    jz .fail
    call ata_reset
    jmp .try
.fail:
    stc
.done:
    pop ax
    ret

; Read sectors of the primary master into memory below 1 MB.
; In:  CL = first LBA (0-255), CH = sector count (1-128),
;      EBP = linear address of the destination.
; Out: CF = 0 on success.
;      CF = 1 on failure, SI = reason, BL = status, BH = error register.
;      Everything else is preserved.
ata_read:
    push eax
    push ecx
    push edx
    push ebp
    push di                     ; console cursor
    push es
    mov ah, ch                  ; sectors left (AL is the port byte)
    mov bh, cl                  ; LBA (BL is the poll mask)

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
    mov al, ah
    out dx, al
    mov al, bh
    mov dx, ATA_LBA_LO
    out dx, al
    xor al, al
    mov dx, ATA_LBA_MID
    out dx, al
    mov dx, ATA_LBA_HI
    out dx, al

    mov dx, ATA_STATUS
    mov al, CMD_READ_SECTORS
    out dx, al

    ; One DRQ block per sector
.sector:
    call ata_delay400
    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov ecx, ebp                ; ES:DI = EBP, normalised so 512 bytes fit
    shr ecx, 4
    mov es, cx
    mov di, bp
    and di, 0x000F
    mov cx, 256
    mov dx, ATA_DATA
    rep insw
    add ebp, 512
    dec ah
    jnz .sector

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
    jmp .out

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
.out:
    pop es
    pop di
    pop ebp
    pop edx
    pop ecx
    pop eax
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
; Preserves every register but the flags.
ata_reset:
    push eax
    push ebx
    push ecx
    push edx
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
    pop edx
    pop ecx
    pop ebx
    pop eax
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
msg_nosig   db 'disk: boot sector has no 0xAA55 signature', 0
msg_readerr_extra db 'disk: read of the extra sectors failed', 0
msg_overlap db 'disk: load overlaps firmware data', 0
msg_range   db 'disk: load address is out of range', 0
msg_status  db '  status=', 0
msg_error   db ' error=', 0
msg_halted  db 'boot halted', 0

; Pad so the reset vector lands at offset 0xFFF0
times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

times 65536 - ($ - $$) db 0
