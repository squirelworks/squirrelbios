; SquirrelBIOS x86 — floppy disk controller (82077-compatible) driver
; Original code. Included by x86/squirrel-bios.asm when CONFIG_FLOPPY.
;
; DMA transfers (8237 channel 2) on the primary FDC at 0x3F0, the result
; phase polled; IRQ 6 stays masked. Media: 1.44 MB (80 cylinders, 2 heads, 18 sectors of 512
; bytes, 500 kbit/s MFM). Drives A: and B: come from CMOS register 0x10
; (high nibble A:, low nibble B:; 0 = none). Public routines:
;
;   fdc_drives   AL = number of drives CMOS reports (0-2)
;   fdc_reset    reset the controller, SPECIFY DMA mode; CF on failure
;   fdc_xfer     one sector between the drive and memory; see below
;
; References: the Intel 82077AA data sheet's command set (RECALIBRATE,
; SEEK, SENSE INTERRUPT STATUS, SPECIFY, READ DATA, WRITE DATA) and the
; IBM PC diskette parameter table layout.
;
; fdc_reset and fdc_xfer expect DS = 0 (they keep state in the BDA).

FDC_DOR         equ 0x3F2       ; digital output: motors, DMA gate, /RESET, drive
FDC_MSR         equ 0x3F4       ; main status (read)
FDC_FIFO        equ 0x3F5       ; data / command FIFO
FDC_CCR         equ 0x3F7       ; configuration control (write): data rate

FDC_MSR_RQM     equ 0x80        ; FIFO ready for a transfer
FDC_MSR_DIO     equ 0x40        ; 1 = controller to CPU
FDC_MSR_BUSY    equ 0x10

FDC_DOR_RESET_N equ 0x04        ; 0 holds the controller in reset
FDC_DOR_DMA     equ 0x08        ; DMA request and IRQ 6 gate
FDC_RATE_500K   equ 0x00        ; CCR value for 1.44 MB media

FDC_CMD_SPECIFY equ 0x03
FDC_CMD_RECAL   equ 0x07
FDC_CMD_SENSEI  equ 0x08
FDC_CMD_SEEK    equ 0x0F
FDC_CMD_READ    equ 0x46        ; MFM, single side
FDC_CMD_WRITE   equ 0x45

FD_CYLS         equ 80
FD_HEADS        equ 2
FD_SPT          equ 18
FD_GAP          equ 0x1B
FD_TIMEOUT      equ 0x00100000  ; MSR polls before giving up

CMOS_FLOPPY     equ 0x10

; 8237 DMA controller, channel 2 (the diskette's)
DMA2_ADDR       equ 0x04
DMA2_COUNT      equ 0x05
DMA_MASK        equ 0x0A        ; single-channel mask
DMA_MODE        equ 0x0B
DMA_CLEARFF     equ 0x0C        ; clear the byte flip-flop
DMA2_PAGE       equ 0x81
DMA_MODE_READ   equ 0x46        ; single, increment, device to memory, ch 2
DMA_MODE_WRITE  equ 0x4A        ; single, increment, memory to device, ch 2

; BIOS data area diskette fields
BDA_FD_RECAL    equ 0x043E      ; bit n: drive n recalibrated since reset
BDA_FD_MOTOR    equ 0x043F      ; bit n: drive n motor on
BDA_FD_STATUS   equ 0x0441      ; last INT 13h status for a diskette

; Diskette status codes (INT 13h AH)
FS_OK           equ 0x00
FS_BAD          equ 0x01
FS_NOMARK       equ 0x02
FS_WPROT        equ 0x03
FS_NOTFOUND     equ 0x04
FS_DMA64K       equ 0x09        ; buffer crosses a 64 KB boundary
FS_CRC          equ 0x10
FS_CTRL         equ 0x20
FS_SEEK         equ 0x40
FS_TIMEOUT      equ 0x80

; AL = number of floppy drives in CMOS 0x10 (A: then B:). ZF set if none.
fdc_drives:
    push bx
    mov al, CMOS_FLOPPY
    call cmos_read
    xor bl, bl
    test al, 0xF0
    jz .b
    inc bl
.b:
    test al, 0x0F
    jz .done
    inc bl
.done:
    mov al, bl
    test al, al
    pop bx
    ret

; Reset the controller: /RESET low then high, sense the four drives'
; reset interrupts, SPECIFY step rate / head times in DMA mode.
; Every register preserved; CF set if the controller does not answer.
fdc_reset:
    push ax
    push cx
    push dx
    mov dx, FDC_DOR
    xor al, al
    out dx, al
    call ata_delay400
    call ata_delay400
    mov al, FDC_DOR_RESET_N | FDC_DOR_DMA
    out dx, al
    mov byte [BDA_FD_RECAL], 0
    mov byte [BDA_FD_MOTOR], 0
    mov dx, FDC_CCR
    mov al, FDC_RATE_500K
    out dx, al
    ; Wait for the controller to come out of reset
    mov cx, 0xFFFF
.ready:
    mov dx, FDC_MSR
    in al, dx
    test al, FDC_MSR_RQM
    jnz .sense
    loop .ready
    jmp .fail
.sense:
    mov cx, 4                   ; one SENSE INTERRUPT per drive after reset
.sensei:
    mov al, FDC_CMD_SENSEI
    call fdc_out
    jc .fail
    call fdc_in                 ; ST0
    jc .fail
    call fdc_in                 ; PCN
    jc .fail
    loop .sensei
    mov al, FDC_CMD_SPECIFY
    call fdc_out
    jc .fail
    mov al, 0xAF                ; SRT = 6 ms, HUT = 240 ms (diskette table)
    call fdc_out
    jc .fail
    mov al, 0x02                ; HLT = 2 ms, ND = 0 (DMA)
    call fdc_out
    jc .fail
    pop dx
    pop cx
    pop ax
    clc
    ret
.fail:
    pop dx
    pop cx
    pop ax
    stc
    ret

; AL -> FIFO once the controller wants a command byte. CF on timeout.
; Preserves every register but the flags.
fdc_out:
    push ecx
    push dx
    push ax
    mov ecx, FD_TIMEOUT
    mov dx, FDC_MSR
.wait:
    in al, dx
    and al, FDC_MSR_RQM | FDC_MSR_DIO
    cmp al, FDC_MSR_RQM
    je .go
    dec ecx
    jnz .wait
    pop ax
    pop dx
    pop ecx
    stc
    ret
.go:
    pop ax
    mov dx, FDC_FIFO
    out dx, al
    pop dx
    pop ecx
    clc
    ret

; AL <- FIFO once the controller has a result byte. CF on timeout.
fdc_in:
    push ecx
    push dx
    mov ecx, FD_TIMEOUT
    mov dx, FDC_MSR
.wait:
    in al, dx
    and al, FDC_MSR_RQM | FDC_MSR_DIO
    cmp al, FDC_MSR_RQM | FDC_MSR_DIO
    je .go
    dec ecx
    jnz .wait
    pop dx
    pop ecx
    stc
    ret
.go:
    mov dx, FDC_FIFO
    in al, dx
    pop dx
    pop ecx
    clc
    ret

; Poll SENSE INTERRUPT STATUS until the seek/recalibrate on drive BL
; ends (ST0 bit 5). Out: AH = ST0, AL = present cylinder; CF on timeout.
fdc_seek_end:
    push ecx
    mov ecx, FD_TIMEOUT
.again:
    mov al, FDC_CMD_SENSEI
    call fdc_out
    jc .fail
    call fdc_in
    jc .fail
    mov ah, al
    call fdc_in
    jc .fail
    cmp ah, 0x80                ; invalid command: nothing pending yet
    je .next
    test ah, 0x20
    jz .next
    mov ch, ah
    and ch, 0x03
    cmp ch, bl
    jne .next
    pop ecx
    clc
    ret
.next:
    dec ecx
    jnz .again
.fail:
    pop ecx
    stc
    ret

; Turn drive BL's motor on (and only that drive's), selecting it.
fdc_motor_on:
    push ax
    push cx
    push dx
    mov cl, bl
    mov al, 0x10
    shl al, cl
    or al, bl
    or al, FDC_DOR_RESET_N | FDC_DOR_DMA
    mov dx, FDC_DOR
    out dx, al
    mov al, 1
    shl al, cl
    mov [BDA_FD_MOTOR], al
    pop dx
    pop cx
    pop ax
    ret

; Move drive BL's heads to cylinder CH (head DH), recalibrating first if
; this drive has not been since the last reset. CF set, AL = status, on
; failure. DS = 0. Preserves every register but AX and the flags.
fdc_seek:
    push cx
    mov cl, bl
    mov al, 1
    shl al, cl
    test [BDA_FD_RECAL], al
    jnz .seek
    or [BDA_FD_RECAL], al
    mov al, FDC_CMD_RECAL
    call fdc_out
    jc .timeout
    mov al, bl
    call fdc_out
    jc .timeout
    call fdc_seek_end
    jc .timeout
.seek:
    mov al, FDC_CMD_SEEK
    call fdc_out
    jc .timeout
    mov al, dh
    shl al, 2
    or al, bl
    call fdc_out
    jc .timeout
    mov al, ch
    call fdc_out
    jc .timeout
    call fdc_seek_end
    jc .timeout
    cmp al, ch
    jne .seekerr
    pop cx
    clc
    ret
.timeout:
    mov byte [BDA_FD_RECAL], 0
    pop cx
    mov al, FS_TIMEOUT
    stc
    ret
.seekerr:
    pop cx
    mov al, FS_SEEK
    stc
    ret

; Transfer one 512-byte sector between drive BL and memory, through ISA
; DMA channel 2 (the 8237 counts the 512 bytes and ends the command).
; In:  BL = drive (0 or 1), CH = cylinder, DH = head, CL = sector (1-18),
;      ES:DI = buffer, AH = 0 read / 1 write.
; Out: CF clear and AL = FS_OK, DI advanced by 512;
;      CF set and AL = diskette status on failure (FS_DMA64K when the
;      buffer crosses a 64 KB physical boundary). Other registers kept.
fdc_xfer:
    push bx
    push cx
    push dx
    push si
    push di
    mov si, ax                  ; SI high byte = direction
    call fdc_dma_setup
    jc .fail
    call fdc_motor_on
    call fdc_seek
    jc .fail
    mov al, FDC_CMD_READ
    test si, 0xFF00
    jz .cmd
    mov al, FDC_CMD_WRITE
.cmd:
    call fdc_out
    jc .timeout
    mov al, dh
    shl al, 2
    or al, bl
    call fdc_out                ; head << 2 | drive
    jc .timeout
    mov al, ch
    call fdc_out                ; C
    jc .timeout
    mov al, dh
    call fdc_out                ; H
    jc .timeout
    mov al, cl
    call fdc_out                ; R
    jc .timeout
    mov al, 2
    call fdc_out                ; N: 512 bytes
    jc .timeout
    mov al, cl
    call fdc_out                ; EOT: stop after this sector
    jc .timeout
    mov al, FD_GAP
    call fdc_out
    jc .timeout
    mov al, 0xFF
    call fdc_out                ; DTL
    jc .timeout

    ; Execution phase runs on DMA; the result phase follows it
    call fdc_in                 ; ST0
    jc .timeout
    mov bh, al
    call fdc_in                 ; ST1
    jc .timeout
    mov bl, al
    call fdc_in                 ; ST2
    jc .timeout
    mov ah, al
    mov cx, 4                   ; C H R N
.rest:
    call fdc_in
    jc .timeout
    loop .rest
    test bh, 0xC0               ; ST0 interrupt code: 00 = normal end
    jz .ok
    ; "End of cylinder" alone is the sector after EOT being asked for once
    ; the count ran out: the transfer itself completed
    cmp bl, 0x80
    jne .error
    test ah, ah
    jz .ok
.error:
    ; Abnormal end: map ST1 / ST2 to a diskette status
    mov al, FS_WPROT
    test bl, 0x02
    jnz .fail
    mov al, FS_NOTFOUND
    test bl, 0x04
    jnz .fail
    test ah, 0x01               ; ST2 missing data address mark
    jnz .fail
    mov al, FS_CRC
    test bl, 0x20
    jnz .fail
    mov al, FS_NOMARK
    test bl, 0x01
    jnz .fail
    mov al, FS_CTRL
    jmp .fail
.ok:
    pop di
    add di, 512
    pop si
    pop dx
    pop cx
    pop bx
    mov al, FS_OK
    clc
    ret
.timeout:
    call fdc_reset              ; leave the controller usable
    mov al, FS_TIMEOUT
.fail:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    stc
    ret

; Program 8237 channel 2 for 512 bytes at ES:DI; SI high byte = 0 for a
; read (device to memory), else a write. CF set, AL = FS_DMA64K, if the
; buffer crosses a 64 KB boundary (the 8237 address wraps there).
; Preserves every register but AX and the flags.
fdc_dma_setup:
    push ebx
    push dx
    xor ebx, ebx
    mov bx, es
    shl ebx, 4
    movzx edx, di
    add ebx, edx                ; physical address
    cmp bx, 0xFE00
    ja .boundary
    mov al, 0x06                ; mask channel 2
    out DMA_MASK, al
    out DMA_CLEARFF, al
    mov al, DMA_MODE_READ
    test si, 0xFF00
    jz .mode
    mov al, DMA_MODE_WRITE
.mode:
    out DMA_MODE, al
    mov al, bl
    out DMA2_ADDR, al
    mov al, bh
    out DMA2_ADDR, al
    shr ebx, 16
    mov al, bl
    out DMA2_PAGE, al
    out DMA_CLEARFF, al
    mov al, 0xFF                ; count - 1 = 511
    out DMA2_COUNT, al
    mov al, 0x01
    out DMA2_COUNT, al
    mov al, 0x02                ; unmask channel 2
    out DMA_MASK, al
    pop dx
    pop ebx
    clc
    ret
.boundary:
    pop dx
    pop ebx
    mov al, FS_DMA64K
    stc
    ret
