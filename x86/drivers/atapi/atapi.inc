; SquirrelBIOS x86 — ATAPI (CD/DVD) PIO driver, primary and secondary IDE
; Original code. Included by x86/squirrel-bios.asm when CONFIG_CDBOOT.
;
; PACKET commands (ATA/ATAPI-4: PACKET A0h, 12-byte CDB, PIO data in DRQ
; blocks whose length the device puts in the byte count registers) with
; the SCSI MMC READ (10) and READ CAPACITY CDBs. Polled, no IRQs.
;
; The unit is named by two bytes in segment 0, which every routine reads
; through FS = 0:
;   CD_BASE  command block base port: 0x1F0 primary, 0x170 secondary
;   CD_SEL   device register value: 0xA0 master, 0xB0 slave
; The same area keeps what El Torito booted (boot/eltorito.inc).

CD_BASE         equ 0x04F0      ; word
CD_SEL          equ 0x04F2      ; byte; 0 until a CD has booted
CD_COUNT        equ 0x04F3      ; word: boot image, 512-byte sectors
CD_IMAGE_LBA    equ 0x04F5      ; dword: boot image, 2048-byte blocks
CD_LOAD_SEG     equ 0x04F9      ; word: where the image was loaded

ATAPI_PRI       equ 0x1F0
ATAPI_SEC       equ 0x170
ATAPI_CTRL_OFF  equ 0x206       ; alternate status = base + 0x206
ATAPI_BLOCK     equ 2048
ATAPI_MAXBLK    equ 32          ; blocks per READ (10) here: 64 KB
ATAPI_TRIES     equ 3           ; the first command may see UNIT ATTENTION

CMD_PACKET      equ 0xA0
SCSI_READ10     equ 0x28
SCSI_READCAP    equ 0x25

; ~400 ns on the selected channel: four alternate status reads.
atapi_delay:
    push ax
    push dx
    mov dx, [fs:CD_BASE]
    add dx, ATAPI_CTRL_OFF
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop dx
    pop ax
    ret

; Wait for BSY to clear. Out: AL = status, CF on timeout. Keeps the rest.
atapi_idle:
    push ecx
    push dx
    mov dx, [fs:CD_BASE]
    add dx, 7
    mov ecx, ATA_TIMEOUT
.poll:
    in al, dx
    test al, ST_BSY
    jz .ok
    dec ecx
    jnz .poll
    pop dx
    pop ecx
    stc
    ret
.ok:
    pop dx
    pop ecx
    clc
    ret

; Is the unit at CD_BASE / CD_SEL an ATAPI device? It is selected and
; asked IDENTIFY DEVICE, which an ATAPI device aborts leaving 14h EBh in
; the LBA mid / high registers. CF clear if so. Keeps every register.
atapi_probe:
    push ax
    push cx
    push dx
    mov dx, [fs:CD_BASE]
    add dx, 7
    in al, dx
    cmp al, 0xFF                ; floating bus: no controller
    je .no
    sub dx, 1
    mov al, [fs:CD_SEL]
    out dx, al
    call atapi_delay
    call atapi_idle
    jc .no
    mov dx, [fs:CD_BASE]
    add dx, 4
    xor al, al
    out dx, al
    inc dx
    out dx, al
    add dx, 2
    mov al, CMD_IDENTIFY
    out dx, al
    call atapi_delay
    in al, dx
    test al, al                 ; nothing behind this select
    jz .no
    call atapi_idle
    jc .no
    test al, ST_ERR
    jz .ata                     ; an ATA disk answered: not ours
    mov dx, [fs:CD_BASE]
    add dx, 4
    in al, dx
    cmp al, 0x14
    jne .no
    inc dx
    in al, dx
    cmp al, 0xEB
    jne .no
    pop dx
    pop cx
    pop ax
    clc
    ret
.ata:
    test al, ST_DRQ
    jz .no
    mov cx, 256                 ; drain its IDENTIFY data
    mov dx, [fs:CD_BASE]
.drain:
    in ax, dx
    loop .drain
.no:
    pop dx
    pop cx
    pop ax
    stc
    ret

; Read CX blocks (1 or more) of 2048 bytes from block EAX to linear
; EBP (below 1 MB), ATAPI_MAXBLK at a time, each command tried
; ATAPI_TRIES times. CF set on failure. Keeps every register.
atapi_read:
    pushad
.chunk:
    mov si, cx
    cmp si, ATAPI_MAXBLK
    jbe .tries
    mov si, ATAPI_MAXBLK
.tries:
    mov bl, ATAPI_TRIES
.again:
    push cx
    mov cx, si
    mov dl, SCSI_READ10
    call atapi_command
    pop cx
    jnc .done
    dec bl
    jnz .again
    popad
    stc
    ret
.done:
    movzx edx, si
    add eax, edx
    shl edx, 11
    add ebp, edx
    sub cx, si
    jnz .chunk
    popad
    clc
    ret

; Number of 2048-byte blocks on the medium (READ CAPACITY's last block
; plus one). Out: CF clear and EAX = blocks. Keeps the other registers.
atapi_capacity:
    push ebp
    push ecx
    push edx
    push ebx
    sub sp, 8
    movzx ebp, sp
    mov dx, ss
    movzx edx, dx
    shl edx, 4
    add ebp, edx                ; the 8 bytes it returns, on the stack
    mov bl, ATAPI_TRIES
.again:
    mov dl, SCSI_READCAP
    call atapi_command
    jnc .got
    dec bl
    jnz .again
    add sp, 8
    pop ebx
    pop edx
    pop ecx
    pop ebp
    stc
    ret
.got:
    mov bx, sp
    mov eax, [ss:bx]            ; last block, big-endian
    call atapi_swap32
    inc eax
    add sp, 8
    pop ebx
    pop edx
    pop ecx
    pop ebp
    clc
    ret

; One PACKET command. DL = SCSI_READ10 (block EAX, CX blocks, CX * 2048
; bytes) or SCSI_READCAP (8 bytes); the data goes to linear EBP.
; CF set on any error. Keeps every register.
atapi_command:
    pushad
    push es
    mov bh, dl                  ; the operation
    movzx esi, cx
    shl esi, 11                 ; bytes expected
    cmp bh, SCSI_READ10
    je .sel
    mov esi, 8
.sel:
    push eax
    call atapi_idle
    pop eax
    jc .fail
    mov dx, [fs:CD_BASE]
    add dx, 6
    push ax
    mov al, [fs:CD_SEL]
    out dx, al
    pop ax
    call atapi_delay
    push eax
    call atapi_idle
    pop eax
    jc .fail
    mov dx, [fs:CD_BASE]
    inc dx                      ; features: PIO
    push ax
    xor al, al
    out dx, al
    add dx, 3                   ; byte count limit: one block
    mov al, ATAPI_BLOCK & 0xFF
    out dx, al
    inc dx
    mov al, ATAPI_BLOCK >> 8
    out dx, al
    add dx, 2
    mov al, CMD_PACKET
    out dx, al
    pop ax
    call atapi_delay
    push eax
    call atapi_wait_drq
    pop eax
    jc .fail
    ; The CDB, 12 bytes as six words, low byte first
    mov dx, [fs:CD_BASE]
    mov edi, eax                ; LBA
    movzx ax, bh
    out dx, ax                  ; opcode, 0
    cmp bh, SCSI_READ10
    je .read10
    xor ax, ax
    out dx, ax
    out dx, ax
    out dx, ax
    out dx, ax
    out dx, ax
    jmp .data
.read10:
    mov eax, edi
    call atapi_swap32           ; big-endian LBA
    out dx, ax                  ; LBA bytes 3, 2
    shr eax, 16
    out dx, ax                  ; LBA bytes 1, 0
    xor al, al                  ; reserved, then count high
    mov ah, ch
    out dx, ax
    mov al, cl
    xor ah, ah
    out dx, ax                  ; count low, 0
    xor ax, ax
    out dx, ax

    ; Data: DRQ blocks of the length in the byte count registers
.data:
    call atapi_delay
    call atapi_idle
    jc .fail
    test al, ST_ERR
    jnz .fail
    test al, ST_DRQ
    jz .end
    mov dx, [fs:CD_BASE]
    add dx, 4
    in al, dx
    mov cl, al
    inc dx
    in al, dx
    mov ch, al                  ; CX = bytes in this block
    test cx, cx
    jz .fail
    movzx ecx, cx
    cmp ecx, esi
    ja .fail
    sub esi, ecx
    mov eax, ebp                ; ES:DI = EBP, normalised
    shr eax, 4
    mov es, ax
    mov di, bp
    and di, 0x000F
    add ebp, ecx
    shr cx, 1
    mov dx, [fs:CD_BASE]
    cld
    rep insw
    jmp .data
.end:
    test esi, esi               ; every byte arrived
    jnz .fail
    pop es
    popad
    clc
    ret
.fail:
    pop es
    popad
    stc
    ret

; After the PACKET command: wait until the device asks for the CDB.
; CF on timeout or error. Clobbers EAX.
atapi_wait_drq:
    push ecx
    push dx
    mov dx, [fs:CD_BASE]
    add dx, 7
    mov ecx, ATA_TIMEOUT
.poll:
    in al, dx
    test al, ST_BSY
    jnz .next
    test al, ST_ERR
    jnz .fail
    test al, ST_DRQ
    jnz .ok
.next:
    dec ecx
    jnz .poll
.fail:
    pop dx
    pop ecx
    stc
    ret
.ok:
    pop dx
    pop ecx
    clc
    ret

; Reverse the bytes of EAX (BSWAP without needing a 486).
atapi_swap32:
    xchg al, ah
    rol eax, 16
    xchg al, ah
    ret
