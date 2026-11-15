; SquirrelBIOS x86 — INT 13h diskette services (DL = 0x00 / 0x01)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_FLOPPY.
;
; int13_handler jumps here with its frame built (pushad, then DS, ES),
; BP = SP + 4, for any DL below 0x80:
;   AH = 0x00  reset the controller
;   AH = 0x02  read AL sectors from CH/DH/CL to ES:BX
;   AH = 0x03  write AL sectors from ES:BX to CH/DH/CL
;   AH = 0x08  drive parameters, ES:DI = diskette parameter table
;   AH = 0x15  drive type (AH = 1: diskette without change line)
; Anything else, or a drive CMOS does not report, returns CF set.

; Diskette parameter table (INT 1Eh points here): 1.44 MB media.
fd_param_table:
    db 0xAF                     ; SRT = 6 ms, HUT = 240 ms
    db 0x02                     ; HLT = 2 ms, DMA
    db 0x25                     ; motor-off delay, ticks
    db 2                        ; 512 bytes per sector
    db FD_SPT                   ; last sector on a track
    db FD_GAP                   ; gap length
    db 0xFF                     ; data length
    db 0x6C                     ; format gap length
    db 0xF6                     ; format fill byte
    db 0x0F                     ; head settle, ms
    db 0x08                     ; motor start, 1/8 s

; AL = drive number. Out: AL = its CMOS 0x10 type nibble, 0 if none.
fdc_drive_type:
    cmp al, 1
    ja .none
    push cx
    mov cl, al
    mov al, CMOS_FLOPPY
    call cmos_read
    test cl, cl
    jnz .b
    shr al, 4
.b:
    and al, 0x0F
    pop cx
    ret
.none:
    xor al, al
    ret

fd_int13:
    xor si, si
    mov ds, si                  ; BDA state lives in segment 0
    mov al, [bp + 20]           ; DL
    call fdc_drive_type
    mov bh, al                  ; drive type, 0 = absent
    mov ah, [bp + 29]
    cmp ah, 0x08
    je .params
    cmp ah, 0x15
    je .type
    test bh, bh
    jz .bad
    cmp ah, 0x00
    je .reset
    cmp ah, 0x02
    je .rw
    cmp ah, 0x03
    je .rw
.bad:
    mov al, FS_BAD
    jmp .fail

.reset:
    call fdc_reset
    jc .timeout
    jmp .ok

.type:
    xor al, al                  ; 0: no such drive
    test bh, bh
    jz .type_out
    mov al, 1                   ; diskette, no change-line support
.type_out:
    mov [bp + 29], al
    and word [bp + 36], 0xFFFE
    jmp .out

.params:
    test bh, bh
    jnz .params_ok
    mov al, ST_PARAMS
    jmp .fail
.params_ok:
    mov word [bp + 28], 0       ; AX = 0
    mov [bp + 16], bh           ; BL = drive type
    mov byte [bp + 17], 0
    mov byte [bp + 25], FD_CYLS - 1
    mov byte [bp + 24], FD_SPT
    mov byte [bp + 21], FD_HEADS - 1
    call fdc_drives
    mov [bp + 20], al           ; DL = number of drives
    mov word [bp + 0], fd_param_table   ; DI
    mov word [bp - 4], 0xF000           ; ES (restored from the frame)
    jmp .ok

.rw:
    movzx cx, byte [bp + 28]    ; AL = count
    test cx, cx
    jz .bad
    mov si, cx                  ; sectors left
    mov ah, [bp + 29]
    sub ah, 2                   ; 0 read, 1 write
    mov [bp + 29], ah           ; keep the direction in the frame for now
    mov bl, [bp + 20]           ; drive
    mov ch, [bp + 25]           ; cylinder
    mov cl, [bp + 24]
    and cl, 0x3F                ; sector
    mov dh, [bp + 21]           ; head
    mov di, [bp + 16]           ; BX = buffer offset in ES
    mov byte [bp + 28], 0       ; sectors done
.next:
    test cl, cl
    jz .notfound
    cmp cl, FD_SPT
    ja .notfound
    cmp dh, FD_HEADS
    jae .notfound
    cmp ch, FD_CYLS
    jae .notfound
    ; Keep ES:DI normalised so a 512-byte transfer never wraps DI
    push dx
    mov ax, di
    shr ax, 4
    mov dx, es
    add ax, dx
    mov es, ax
    and di, 0x000F
    pop dx
    push word 2                 ; attempts, at [bp - 6]
.try:
    mov ah, [bp + 29]
    call fdc_xfer
    jnc .sector_ok
    cmp al, FS_DMA64K           ; the caller's buffer: no use retrying
    je .give_up
    call fdc_reset              ; e.g. the first access after a media change
    dec word [bp - 6]
    jnz .try
.give_up:
    add sp, 2
    jmp .fail
.sector_ok:
    add sp, 2
    inc byte [bp + 28]
    dec si
    jz .ok_rw
    inc cl                      ; next sector, then head, then cylinder
    cmp cl, FD_SPT
    jbe .same
    mov cl, 1
    inc dh
    cmp dh, FD_HEADS
    jb .same
    xor dh, dh
    inc ch
.same:
    jmp .next

.notfound:
    mov al, FS_NOTFOUND
    jmp .fail
.timeout:
    mov al, FS_TIMEOUT
.fail:
    mov [bp + 29], al
    mov [BDA_FD_STATUS], al
    or word [bp + 36], 1
    jmp .out
.ok_rw:
.ok:
    mov byte [bp + 29], FS_OK
    mov byte [BDA_FD_STATUS], FS_OK
    and word [bp + 36], 0xFFFE
.out:
    pop es
    pop ds
    popad
    iret
