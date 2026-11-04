; SquirrelBIOS x86 — INT 13h disk services
; Original code. Included by x86/squirrel-bios.asm.

; INT 13h. pushad frame, SP = BP:
;   [bp+28] EAX (AL at +28, AH at +29)
;   [bp+24] ECX (CL at +24, CH at +25)
;   [bp+20] EDX (DL at +20, DH at +21)
;   [bp+16] EBX
;   [bp+36] FLAGS
; Primary master (DL = 0x80) and primary slave (DL = 0x81) are drives.
; CF follows the status in AH: clear on success, set on failure.
int13_handler:
    pushad
    mov bp, sp
    push ds
    push es
    cmp ah, 0x00
    je .reset
    cmp ah, 0x02
    je .read_chs
    cmp ah, 0x03
    je .write_chs
    cmp ah, 0x08
    je .params
    cmp ah, 0x42
    je .read_ext
    cmp ah, 0x43
    je .write_ext
    jmp .invalid

.reset:
    call disk_set_dev
    jc .invalid
    call ata_reset
    jc .reset_fail
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out
.reset_fail:
    mov byte [bp + 29], ST_RESET
    or word [bp + 36], 1
    jmp .out

.params:
    call disk_set_dev
    jc .invalid
    mov al, 1                       ; ATA_PRESENT bit for this unit
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .params_bit
    mov al, 2
.params_bit:
    test [ss:ATA_PRESENT], al
    jz .params_absent
    mov byte [bp + 29], ST_OK
    mov byte [bp + 25], DISK_CYL_LO
    mov byte [bp + 24], DISK_CYL_HI_CL
    mov byte [bp + 21], DISK_HEADS - 1
    mov al, [ss:BDA_HDCOUNT]
    mov [bp + 20], al               ; hard disks present
    and word [bp + 36], 0xFFFE
    jmp .out
.params_absent:
    mov byte [bp + 29], ST_PARAMS   ; drive parameter activity failed
    or word [bp + 36], 1
    jmp .out

.read_ext:
    call disk_parse_packet
    jc .invalid
    push bp
    mov ebp, edx
    call disk_clear_stale
    call ata_read                   ; EAX = LBA, CL = count, EBP = dest
    pop bp
    jc .ioerr
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.write_ext:
    call disk_parse_packet
    jc .invalid
    push bp
    mov ebp, edx
    call disk_clear_stale
    call ata_write
    pop bp
    jc .ioerr
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.read_chs:
    call disk_set_dev
    jc .invalid
    movzx esi, byte [bp + 28]       ; AL = count
    test si, si
    jz .invalid
    cmp si, 128
    ja .invalid
    movzx eax, byte [bp + 24]       ; CL
    and al, 0x3F
    jz .notfound
    cmp al, DISK_SPT
    ja .notfound
    movzx ebx, al                   ; sector (1-63)
    movzx eax, byte [bp + 24]
    shr ax, 6
    and ax, 3
    shl ax, 8
    movzx edx, byte [bp + 25]       ; CH
    or eax, edx                     ; cylinder
    movzx edx, byte [bp + 21]       ; DH = head
    cmp dx, DISK_HEADS
    jae .notfound
    push ebx                        ; sector
    push edx                        ; head
    mov ebx, DISK_HEADS
    mul ebx
    pop edx
    add eax, edx
    mov ebx, DISK_SPT
    mul ebx
    pop edx                         ; sector
    dec edx
    add eax, edx                    ; LBA
    movzx edx, word [bp + 16]       ; BX = buffer offset
    mov bx, es
    movzx ebx, bx
    shl ebx, 4
    add edx, ebx                    ; linear destination
    mov ebx, esi
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .invalid
    cmp ebx, LOAD_LIMIT
    ja .invalid
    push bp
    mov ebp, edx
    mov ecx, esi                    ; CL = count
    call disk_clear_stale
    call ata_read
    pop bp
    jc .ioerr_chs
    mov byte [bp + 29], ST_OK       ; AL stays the count we transferred
    and word [bp + 36], 0xFFFE
    jmp .out

.write_chs:
    call disk_set_dev
    jc .invalid
    movzx esi, byte [bp + 28]
    test si, si
    jz .invalid
    cmp si, 128
    ja .invalid
    movzx eax, byte [bp + 24]
    and al, 0x3F
    jz .notfound
    cmp al, DISK_SPT
    ja .notfound
    movzx ebx, al
    movzx eax, byte [bp + 24]
    shr ax, 6
    and ax, 3
    shl ax, 8
    movzx edx, byte [bp + 25]
    or eax, edx
    movzx edx, byte [bp + 21]
    cmp dx, DISK_HEADS
    jae .notfound
    push ebx
    push edx
    mov ebx, DISK_HEADS
    mul ebx
    pop edx
    add eax, edx
    mov ebx, DISK_SPT
    mul ebx
    pop edx
    dec edx
    add eax, edx
    movzx edx, word [bp + 16]
    mov bx, es
    movzx ebx, bx
    shl ebx, 4
    add edx, ebx
    mov ebx, esi
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .invalid
    cmp ebx, LOAD_LIMIT
    ja .invalid
    push bp
    mov ebp, edx
    mov ecx, esi
    call disk_clear_stale
    call ata_write
    pop bp
    jc .ioerr_chs
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.ioerr_chs:
    mov byte [bp + 28], 0           ; no sector transferred
.ioerr:
    call disk_status
    mov [bp + 29], al
    or word [bp + 36], 1
    jmp .out

.notfound:
    mov byte [bp + 28], 0
    mov byte [bp + 29], ST_NOTFOUND
    or word [bp + 36], 1
    jmp .out

.invalid:
    mov byte [bp + 29], ST_BAD
    or word [bp + 36], 1
.out:
    pop es
    pop ds
    popad
    iret

; IN: SI = ata_read failure reason. OUT: AL = INT 13h status. Preserves SI.
disk_status:
    mov al, ST_BAD
    cmp si, msg_readerr
    jne .timeout
    mov al, ST_NOTFOUND
    ret
.timeout:
    cmp si, msg_timeout
    jne .nodrive
    mov al, ST_TIMEOUT
    ret
.nodrive:
    cmp si, msg_nodrive
    je .notready
    cmp si, msg_nodrive_slave
    jne .done
.notready:
    mov al, ST_NOTREADY
.done:
    ret

; Map DL (at [bp+20]) to ATA_DEVSEL. CF set if not 0x80 or 0x81.
disk_set_dev:
    mov al, [bp + 20]
    cmp al, 0x80
    je .master
    cmp al, 0x81
    je .slave
    stc
    ret
.master:
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    clc
    ret
.slave:
    mov byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    clc
    ret

; Parse a 16-byte disk address packet at DS:SI.
; Out: CF clear, EAX = LBA, CL = count, EDX = linear buffer; CF set if bad.
disk_parse_packet:
    call disk_set_dev
    jc .bad
    cmp byte [si], 0x10
    jne .bad
    movzx ecx, word [si + 2]
    test cx, cx
    jz .bad
    cmp cx, 128
    ja .bad
    cmp dword [si + 12], 0
    jne .bad
    mov eax, [si + 8]
    test eax, 0xF0000000
    jnz .bad
    movzx edx, word [si + 6]
    shl edx, 4
    movzx ebx, word [si + 4]
    add edx, ebx
    mov ebx, ecx
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .bad
    cmp ebx, LOAD_LIMIT
    ja .bad
    clc
    ret
.bad:
    stc
    ret

; A failed command leaves ERR set, and ata_read refuses to start another
; while that bit is set. Drop it before a guest read. Preserves registers.
disk_clear_stale:
    push ax
    push dx
    mov dx, ATA_STATUS
    in al, dx
    test al, ST_ERR | ST_DF
    jz .ok
    call ata_reset
.ok:
    pop dx
    pop ax
    ret
