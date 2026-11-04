; SquirrelBIOS x86 — ATA PIO driver (primary channel)
; Original code. Included by x86/squirrel-bios.asm.

; Read with up to ATA_ATTEMPTS attempts, soft reset between them.
; In/Out: as ata_read. Does not touch EAX, ECX or EBP, so a retry
; repeats the same read. The attempt count lives in the BIOS data area
; because EBP is the caller's buffer address.
ata_read_retry:
    mov byte [ss:ATA_RETRY_CNT], ATA_ATTEMPTS
.try:
    call ata_read
    jnc .done
    cmp si, msg_nodrive         ; nothing to retry against
    je .fail
    cmp si, msg_nodrive_slave
    je .fail
    cmp si, msg_atapi
    je .fail
    cmp si, msg_atapi_slave
    je .fail
    dec byte [ss:ATA_RETRY_CNT]
    jz .fail
    call ata_reset
    jmp .try
.fail:
    stc
.done:
    ret

; Read sectors of the selected primary drive into memory below 1 MB.
; ATA_DEVSEL is master (0xE0) or slave (0xF0). In/Out as before.
; In:  EAX = LBA; a range that reaches 2^28 or above is read with the
;      LBA48 command (READ SECTORS EXT), anything below with LBA28,
;      CL  = sector count (1-128),
;      EBP = linear address of the destination.
; Out: CF = 0 on success. Every register preserved.
;      CF = 1 on failure, SI = reason, BL = status, BH = error register.
;      EAX, ECX, EDX, EBP, EDI and ES are preserved either way.
ata_read:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push es
    movzx esi, cl               ; sectors left
    mov edi, eax                ; LBA

    test si, si
    jz .badarg
    cmp si, 128
    ja .badarg
    movzx eax, si               ; the last sector must still be below 2^32
    dec eax
    add eax, edi
    jc .badarg

    ; 0xFF is a floating bus: no controller on the primary channel
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive_bus

    mov dx, ATA_DRIVE
    mov eax, edi
    shr eax, 24
    and al, 0x0F
    mov ah, [ss:ATA_DEVSEL]
    or al, ah                   ; master or slave, LBA
    out dx, al
    call ata_delay400

    ; 0x00 or 0xFF after selecting: nothing answered on that unit,
    ; unless an ATAPI device left its signature in the LBA registers.
    ; mid/hi both 0xFF with a ready status is QEMU's empty-unit quirk
    ; when the other device on the cable is the only one present.
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive
    test al, al
    jz .check_sig
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0xFF
    jne .present
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xFF
    je .nodrive
    jmp .present
.check_sig:
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

    mov ax, CMD_READ_EXT << 8 | CMD_READ_SECTORS
    call ata_issue

    cld
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
    dec si
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
    jmp .ok

.badarg:
    mov si, msg_readerr
    xor bx, bx
    jmp .fail
.nodrive_bus:
    mov si, msg_nodrive         ; floating bus: always the master wording
    jmp .failed
.nodrive:
    mov si, msg_nodrive
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_nodrive_slave
    jmp .failed
.atapi:
    mov si, msg_atapi
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_atapi_slave
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
.fail:
    pop es
    add sp, 8                   ; drop saved EBX and ESI; BL/BH/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc                         ; ADD above clears CF, so set it last
    ret
.ok:
    pop es
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret

; Write sectors of the selected primary drive from memory below 1 MB.
; Same register convention as ata_read.
ata_write:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push es
    movzx esi, cl
    mov edi, eax

    test si, si
    jz .badarg
    cmp si, 128
    ja .badarg
    movzx eax, si               ; the last sector must still be below 2^32
    dec eax
    add eax, edi
    jc .badarg

    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive_bus

    mov dx, ATA_DRIVE
    mov eax, edi
    shr eax, 24
    and al, 0x0F
    mov ah, [ss:ATA_DEVSEL]
    or al, ah
    out dx, al
    call ata_delay400

    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive
    test al, al
    jz .check_sig
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0xFF
    jne .present
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xFF
    je .nodrive
    jmp .present
.check_sig:
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

    mov ax, CMD_WRITE_EXT << 8 | CMD_WRITE_SECTORS
    call ata_issue

    cld
.sector:
    call ata_delay400
    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov ecx, ebp
    shr ecx, 4
    mov es, cx
    mov di, bp
    and di, 0x000F
    push ds
    push si
    mov si, di
    mov ax, es
    mov ds, ax
    mov cx, 256
    mov dx, ATA_DATA
    rep outsw
    pop si
    pop ds
    add ebp, 512
    dec si
    jnz .sector

    call ata_delay400
    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error
    test al, ST_DRQ
    jnz .error
    clc
    jmp .ok

.badarg:
    mov si, msg_readerr
    xor bx, bx
    jmp .fail
.nodrive_bus:
    mov si, msg_nodrive
    jmp .failed
.nodrive:
    mov si, msg_nodrive
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_nodrive_slave
    jmp .failed
.atapi:
    mov si, msg_atapi
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_atapi_slave
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
.fail:
    pop es
    add sp, 8
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret
.ok:
    pop es
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret

; Load the taskfile with LBA EDI and count SI (1-128), then send the
; command: AL (LBA28) when the last sector is below 2^28, otherwise AH
; (LBA48, the "previous" high bytes first). The drive is selected already.
; Clobbers EAX and DX.
ata_issue:
    push ecx
    movzx ecx, si
    dec ecx
    add ecx, edi
    cmp ecx, 0x0FFFFFFF
    pop ecx
    jbe .lba28
    push ax
    mov dx, ATA_DRIVE           ; LBA48: device register bits 3:0 are 0
    mov al, [ss:ATA_DEVSEL]
    out dx, al
    xor al, al
    mov dx, ATA_COUNT           ; count bits 15:8
    out dx, al
    mov eax, edi
    shr eax, 24
    mov dx, ATA_LBA_LO          ; LBA bits 31:24
    out dx, al
    xor al, al
    mov dx, ATA_LBA_MID         ; LBA bits 39:32
    out dx, al
    mov dx, ATA_LBA_HI          ; LBA bits 47:40
    out dx, al
    call .low
    pop ax
    mov al, ah
    jmp .command
.lba28:
    call .low
.command:
    mov dx, ATA_STATUS
    out dx, al
    ret
; Count bits 7:0 and LBA bits 23:0. Keeps AL.
.low:
    push ax
    mov dx, ATA_COUNT
    mov ax, si
    out dx, al
    mov eax, edi
    mov dx, ATA_LBA_LO
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_MID
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_HI
    out dx, al
    pop ax
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

; Is there an ATA disk at ATA_DEVSEL? Select it, wait for BSY to drop,
; then IDENTIFY DEVICE and read the status that command produces, so no
; status left over from an earlier transfer or from the other unit is
; trusted. The 512 IDENTIFY bytes are read and dropped.
; Out: CF = 0 when an ATA disk answered. CF = 1 otherwise, with SI =
;      msg_nodrive / msg_atapi (master or slave wording), BL = status,
;      BH = error register, as ata_read reports them.
;      Every other register preserved.
ata_probe:
    call ata_probe_cmd
    jc .ret
    push ax
    push cx
    push dx
    mov cx, 256                 ; drain the IDENTIFY data block
    mov dx, ATA_DATA
.drain:
    in ax, dx
    loop .drain
    pop dx
    pop cx
    pop ax
    clc
.ret:
    ret

; ata_probe up to the point where the 512 IDENTIFY bytes wait in the data
; register (CF clear); the caller reads them. Same outputs otherwise.
ata_probe_cmd:
    push eax
    push ecx
    push edx
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF                ; floating bus: no controller at all
    je .nodrive
    mov dx, ATA_DRIVE
    mov al, [ss:ATA_DEVSEL]
    out dx, al
    call ata_delay400
    mov ecx, ATA_TIMEOUT        ; wait for BSY to drop on this unit
    mov dx, ATA_STATUS
.idle:
    in al, dx
    test al, ST_BSY
    jz .idle_ok
    dec ecx
    jnz .idle
    jmp .nodrive
.idle_ok:
    xor al, al                  ; IDENTIFY ignores these; clear them so
    mov dx, ATA_COUNT           ; the signature read below is fresh
    out dx, al
    mov dx, ATA_LBA_LO
    out dx, al
    mov dx, ATA_LBA_MID
    out dx, al
    mov dx, ATA_LBA_HI
    out dx, al
    mov dx, ATA_STATUS
    mov al, CMD_IDENTIFY
    out dx, al
    call ata_delay400
    in al, dx
    test al, al                 ; 0: no device behind this select
    jz .nodrive
    cmp al, 0xFF
    je .nodrive
    push bx
    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    pop bx
    jc .nodrive
    test al, ST_ERR | ST_DF
    jnz .aborted
    pop edx                     ; the data block is ready to read
    pop ecx
    pop eax
    clc
    ret
.aborted:
    ; ATAPI devices abort IDENTIFY DEVICE and leave 14h EBh behind
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0x14
    jne .nodrive
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xEB
    jne .nodrive
    mov si, msg_atapi
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .regs
    mov si, msg_atapi_slave
    jmp .regs
.nodrive:
    mov si, msg_nodrive
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .regs
    mov si, msg_nodrive_slave
.regs:
    mov dx, ATA_STATUS
    in al, dx
    mov bl, al
    mov dx, ATA_ERROR
    in al, dx
    mov bh, al
    pop edx
    pop ecx
    pop eax
    stc
    ret

; Probe the primary master and slave once at POST. Sets ATA_PRESENT
; (bit 0 master, bit 1 slave) and the BDA hard-disk count at 0x475.
; ATA_DEVSEL is left on the master. Every register preserved.
ata_detect:
    push ax
    push bx
    push si
    xor ah, ah
    mov byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    call ata_probe
    jc .master
    or ah, 2
.master:
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    call ata_probe
    jc .done
    or ah, 1
.done:
    mov [ss:ATA_PRESENT], ah
    mov al, ah
    shr al, 1
    and ah, 1
    add al, ah
    mov [ss:BDA_HDCOUNT], al
    pop si
    pop bx
    pop ax
    ret

; Size of the ATA disk at ATA_DEVSEL from IDENTIFY DEVICE: words 100-103
; when word 83 bit 10 says LBA48 is supported, else words 60-61.
; Out: CF clear and EDX:EAX = sectors; CF set if the unit does not
; answer as an ATA disk. Keeps every other register.
ata_capacity:
    push ebx
    push ecx
    push esi
    push edi
    call ata_probe_cmd
    jc .fail
    xor ecx, ecx                ; word index
    xor esi, esi                ; words 60-61
    xor edi, edi                ; words 100-101
    xor ebx, ebx                ; BX = words 102, word 83 in the high half
    mov dx, ATA_DATA
.word:
    in ax, dx
    cmp cl, 60
    jne .w61
    mov si, ax
.w61:
    cmp cl, 61
    jne .w83
    ror esi, 16
    mov si, ax
    ror esi, 16
.w83:
    cmp cl, 83
    jne .w100
    ror ebx, 16
    mov bx, ax
    ror ebx, 16
.w100:
    cmp cl, 100
    jne .w101
    mov di, ax
.w101:
    cmp cl, 101
    jne .w102
    ror edi, 16
    mov di, ax
    ror edi, 16
.w102:
    cmp cl, 102
    jne .wnext
    mov bx, ax                  ; word 103 is 0 below 2^48 sectors
.wnext:
    inc cx
    cmp cx, 256
    jb .word
    mov eax, esi
    xor edx, edx
    test ebx, 0x04000000        ; word 83 bit 10: LBA48 feature set
    jz .done
    mov eax, edi
    movzx edx, bx
.done:
    pop edi
    pop esi
    pop ecx
    pop ebx
    clc
    ret
.fail:
    pop edi
    pop esi
    pop ecx
    pop ebx
    stc
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
