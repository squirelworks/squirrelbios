; SquirrelBIOS x86 — INT 13h for the El Torito boot CD (DL = CD_DRIVE)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_CDBOOT.
;
; int13_handler jumps here with its frame built (pushad, then DS, ES),
; BP = SP + 4, when DL = CD_DRIVE and a CD booted. Blocks are 2048 bytes.
;   AH = 0x00  reset: CF clear
;   AH = 0x41  BX = 55AAh: BX = AA55h, AH = 30h, CX = 0001h
;   AH = 0x42  extended read: packet at DS:SI as for a hard disk, the
;              count (1-32) and LBA in 2048-byte blocks
;   AH = 0x48  parameters: 1Ah or 1Eh bytes, 2048 bytes per block,
;              total blocks from READ CAPACITY
;   AH = 0x4B  AL = 00h or 01h: the 13h-byte specification packet to
;              DS:SI (no emulation, so terminating changes nothing)
; Anything else returns CF set and AH = 01h.

cd_int13:
    push fs
    xor ax, ax
    mov fs, ax
    mov ah, [bp + 29]
    cmp ah, 0x00
    je .ok
    cmp ah, 0x41
    je .check
    cmp ah, 0x42
    je .read
    cmp ah, 0x48
    je .params
    cmp ah, 0x4B
    je .spec
.bad:
    mov byte [bp + 29], ST_BAD
    or word [bp + 36], 1
    jmp .out
.fail:
    mov byte [bp + 29], ST_NOTFOUND
    or word [bp + 36], 1
    jmp .out
.ok:
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
.out:
    pop fs
    pop es
    pop ds
    popad
    iret

.check:
    cmp word [bp + 16], 0x55AA
    jne .bad
    mov word [bp + 16], 0xAA55
    mov word [bp + 24], EDD_SUBSET_FIXED
    mov byte [bp + 29], EDD_VERSION
    and word [bp + 36], 0xFFFE
    jmp .out

.read:
    cmp byte [si], 0x10
    jb .bad
    movzx ecx, word [si + 2]
    test cx, cx
    jz .bad
    cmp cx, ATAPI_MAXBLK
    ja .bad
    cmp dword [si + 12], 0
    jne .bad
    movzx ebp, word [si + 6]
    shl ebp, 4
    movzx eax, word [si + 4]
    add ebp, eax                ; linear buffer
    mov edx, ecx
    shl edx, 11
    add edx, ebp
    cmp edx, LOAD_LIMIT
    ja .bad_bp
    mov eax, [si + 8]
    call atapi_read
    mov bp, sp                  ; the frame: FS, ES, DS, then pushad
    lea bp, [bp + 6]
    jc .fail
    jmp .ok
.bad_bp:
    mov bp, sp
    lea bp, [bp + 6]
    jmp .bad

.params:
    mov ax, [si]
    cmp ax, 0x1A
    jb .bad
    call atapi_capacity
    jc .fail
    mov [si + 16], eax          ; total blocks
    mov dword [si + 20], 0
    mov word [si + 2], 0        ; flags: no CHS
    mov dword [si + 4], 0
    mov dword [si + 8], 0
    mov dword [si + 12], 0
    mov word [si + 24], ATAPI_BLOCK
    mov cx, 0x1A
    cmp word [si], 0x1E
    jb .params_size
    mov dword [si + 26], 0xFFFFFFFF ; no DPTE
    mov cx, 0x1E
.params_size:
    mov [si], cx
    jmp .ok

.spec:
    cmp byte [bp + 28], 0x01
    ja .bad
    mov byte [si], 0x13
    mov byte [si + 1], 0x00     ; no emulation
    mov byte [si + 2], CD_DRIVE
    xor al, al                  ; controller: 0 primary, 1 secondary
    cmp word [fs:CD_BASE], ATAPI_PRI
    je .ctrl
    inc al
.ctrl:
    mov [si + 3], al
    mov eax, [fs:CD_IMAGE_LBA]
    mov [si + 4], eax
    xor ax, ax                  ; device: bit 0 = slave
    cmp byte [fs:CD_SEL], 0xB0
    jne .dev
    inc ax
.dev:
    mov [si + 8], ax
    mov word [si + 10], 0       ; no user buffer
    mov ax, [fs:CD_LOAD_SEG]
    mov [si + 12], ax
    mov ax, [fs:CD_COUNT]
    mov [si + 14], ax
    mov byte [si + 16], 0       ; CHS: not used without emulation
    mov word [si + 17], 0
    jmp .ok

; --- Floppy emulation ----------------------------------------------------
;
; A boot entry of media type 1, 2 or 3 is a 1.2, 1.44 or 2.88 MB diskette
; image on the CD. It becomes drive 00h: int13_handler sends DL = 00h
; here while CD_EMUL is set, and every 512-byte sector is read from its
; 2048-byte block through a bounce block in the EBDA (free when a CD
; boots: the CD is tried only when no IDE, SATA, NVMe, USB or virtio disk
; answered, the drivers that keep queues there). The image is read-only.
; A real drive A: stays reachable as 01h only through its own controller
; state, so it is not offered: drive 01h is left to fd_int13 as before.
;   AH = 00h  reset: CF clear
;   AH = 01h  status of the last call (always 00h here)
;   AH = 02h  read AL sectors from CH/CL/DH to ES:BX
;   AH = 03h  write: AH = 03h (write-protected)
;   AH = 08h  parameters: BL = drive type, CH/CL/DH the geometry, ES:DI
;             the diskette parameter table INT 1Eh points to
;   AH = 15h  AH = 01h, a diskette without change line
;   AH = 16h  AH = 00h, not changed
;   AH = 4Bh  AL = 00h terminate the emulation / 01h status: the 13h-byte
;             specification packet to DS:SI

CD_EMUL         equ 0x00E4      ; EBDA byte: 0, or the emulated media (1-4)
CD_HD_SPT       equ 0x00E8      ; EBDA byte: media 4, sectors per track
CD_HD_HEADS     equ 0x00E9      ; EBDA byte: heads
CD_HD_CYLS      equ 0x00EA      ; EBDA word: cylinders
CD_HD_TOTAL     equ 0x00EC      ; EBDA dword: sectors
CD_HD_DRIVE     equ 0x80
CD_BOUNCE       equ 0x1000      ; EBDA offset: one 2048-byte block
CD_EMUL_DRIVE   equ 0x00
ST_WRPROT       equ 0x03

; Media 1-3: sectors per track, cylinders, drive type for AH=08h
cd_emu_geom:
    db 15, 80, 2                ; 1.2 MB
    db 18, 80, 4                ; 1.44 MB
    db 36, 80, 6                ; 2.88 MB

; CF clear when DL is the emulated diskette (00h, media 1-3) or hard
; disk (80h, media 4). Keeps every register.
cd_emu_owns:
    cmp byte [ss:CD_SEL], 0
    je .no
    push ax
    push ds
    push word EBDA_BASE >> 4
    pop ds
    mov al, [CD_EMUL]
    pop ds
    test al, al
    jz .no_ax
    cmp al, 4
    je .hd
    cmp dl, CD_EMUL_DRIVE
    jne .no_ax
    pop ax
    clc
    ret
.hd:
    cmp dl, CD_HD_DRIVE
    jne .no_ax
    pop ax
    clc
    ret
.no_ax:
    pop ax
.no:
    stc
    ret

; SI = the geometry row of the emulated media. Keeps every other register.
cd_emu_row:
    push ax
    push ds
    push word EBDA_BASE >> 4
    pop ds
    movzx si, byte [CD_EMUL]
    pop ds
    dec si
    mov ax, si
    shl si, 1
    add si, ax
    add si, cd_emu_geom
    pop ax
    ret

; Read 512-byte sector EAX of the image to ES:DI. CF set on a read error.
; FS = 0. Keeps every register.
cd_emu_sector:
    pushad
    push ds
    mov ebx, eax
    shr eax, 2
    add eax, [fs:CD_IMAGE_LBA]
    mov cx, 1
    mov ebp, EBDA_BASE + CD_BOUNCE
    call atapi_read
    jc .out
    and bx, 3
    shl bx, 9
    lea si, [bx + CD_BOUNCE]
    push word EBDA_BASE >> 4
    pop ds
    mov cx, 256
    cld
    rep movsw
    clc
.out:
    pop ds
    popad
    ret

; INT 13h for DL = 00h while the emulation is on: the frame as for
; cd_int13 (pushad, DS, ES; BP = SP + 4).
cd_emu_int13:
    cmp dl, CD_HD_DRIVE
    je cd_hd_int13
    push fs
    xor ax, ax
    mov fs, ax
    mov ah, [bp + 29]
    cmp ah, 0x00
    je .ok
    cmp ah, 0x01
    je .ok
    cmp ah, 0x02
    je .read
    cmp ah, 0x03
    je .wrprot
    cmp ah, 0x08
    je .params
    cmp ah, 0x15
    je .type
    cmp ah, 0x16
    je .ok
    cmp ah, 0x4B
    je .spec
.bad:
    mov byte [bp + 29], ST_BAD
    jmp .fail
.wrprot:
    mov byte [bp + 29], ST_WRPROT
    jmp .fail
.notfound:
    mov byte [bp + 29], ST_NOTFOUND
.fail:
    or word [bp + 36], 1
    jmp .out
.ok:
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
.out:
    pop fs
    pop es
    pop ds
    popad
    iret

.type:
    mov byte [bp + 29], 0x01
    and word [bp + 36], 0xFFFE
    jmp .out

.params:
    call cd_emu_row
    mov word [bp + 28], 0               ; AX = 0
    mov al, [cs:si + 2]
    mov [bp + 16], al                   ; BL = drive type
    mov al, [cs:si + 1]
    dec al
    mov [bp + 25], al                   ; CH = last cylinder
    mov al, [cs:si]
    mov [bp + 24], al                   ; CL = sectors per track
    mov byte [bp + 21], 1               ; DH = last head
    mov byte [bp + 20], 1               ; DL = drives
    mov ax, [fs:0x1E * 4]
    mov [bp + 0], ax                    ; DI
    mov ax, [fs:0x1E * 4 + 2]
    mov [bp - 4], ax                    ; ES (restored from the frame)
    jmp .ok

.read:
    call cd_emu_row
    movzx cx, byte [bp + 28]            ; AL = count
    jcxz .bad
    mov byte [bp + 28], 0
    mov di, [bp + 16]                   ; ES:BX, the caller's ES
.sector:
    ; LBA = (cylinder * 2 + head) * spt + sector - 1
    mov al, [bp + 24]
    and al, 0x3F
    jz .notfound
    cmp al, [cs:si]
    ja .notfound
    movzx ax, byte [bp + 25]
    mov dl, [bp + 24]
    shr dl, 6
    mov ah, dl                          ; 10-bit cylinder
    cmp al, [cs:si + 1]
    jae .notfound
    test ah, ah
    jnz .notfound
    cmp byte [bp + 21], 1
    ja .notfound
    shl ax, 1
    add al, [bp + 21]
    adc ah, 0
    movzx edx, byte [cs:si]
    movzx eax, ax
    imul eax, edx
    movzx edx, byte [bp + 24]
    and dl, 0x3F
    dec dx
    add eax, edx
    ; ES:DI normalised so the 512 bytes never wrap DI
    push ax
    mov ax, di
    shr ax, 4
    mov dx, es
    add ax, dx
    mov es, ax
    and di, 0x000F
    pop ax
    call cd_emu_sector
    jc .notfound
    add di, 512
    inc byte [bp + 28]
    ; Next sector, then head, then cylinder
    mov al, [bp + 24]
    and al, 0x3F
    inc al
    cmp al, [cs:si]
    jbe .same_track
    mov al, 1
    xor byte [bp + 21], 1
    jnz .same_track
    inc byte [bp + 25]
.same_track:
    and byte [bp + 24], 0xC0
    or [bp + 24], al
    dec cx
    jnz .sector
    jmp .ok

.spec:
    cmp byte [bp + 28], 0x01
    ja .bad
    push ds
    push word EBDA_BASE >> 4
    pop ds
    mov al, [CD_EMUL]
    cmp byte [bp + 28], 0
    jne .keep
    mov byte [CD_EMUL], 0               ; terminate: 00h is a real drive again
.keep:
    pop ds
    mov byte [si], 0x13
    mov [si + 1], al                    ; media type
    mov byte [si + 2], CD_EMUL_DRIVE
    xor al, al
    cmp word [fs:CD_BASE], ATAPI_PRI
    je .ctrl
    inc al
.ctrl:
    mov [si + 3], al
    mov eax, [fs:CD_IMAGE_LBA]
    mov [si + 4], eax
    xor ax, ax
    cmp byte [fs:CD_SEL], 0xB0
    jne .dev
    inc ax
.dev:
    mov [si + 8], ax
    mov word [si + 10], 0
    mov ax, [fs:CD_LOAD_SEG]
    mov [si + 12], ax
    mov ax, [fs:CD_COUNT]
    mov [si + 14], ax
    push si
    call cd_emu_row
    mov al, [cs:si + 1]
    dec al
    mov ah, [cs:si]
    pop si
    mov [si + 16], ah                   ; sectors per track (and cyl bits 8-9 = 0)
    mov [si + 17], al                   ; last cylinder
    mov byte [si + 18], 1               ; last head
    jmp .ok

; --- Hard disk emulation -------------------------------------------------
;
; A boot entry of media type 4 is a hard disk image: drive 80h, read-only,
; with the geometry cd_hd_geometry found (CD_HD_*), sectors read through
; cd_emu_sector as for the diskette.
;   AH = 00h, 0Dh, 10h, 11h  reset, ready, recalibrate: CF clear
;   AH = 01h  status of the last call (always 00h here)
;   AH = 02h  read AL sectors from CH/CL/DH to ES:BX
;   AH = 03h, 43h  write: AH = 03h (write-protected)
;   AH = 04h, 0Ch, 44h  verify, seek: CF clear
;   AH = 08h  parameters: CH/CL/DH the last cylinder, sector and head,
;             DL = 1 drive
;   AH = 15h  AH = 03h, a hard disk of CX:DX sectors
;   AH = 41h  EDD 3.0, fixed disk access subset
;   AH = 42h  read the packet's sectors to its buffer
;   AH = 48h  parameters: CHS, sectors, 512 bytes per sector
;   AH = 4Bh  AL = 00h terminate the emulation / 01h status: the 13h-byte
;             specification packet to DS:SI

; INT 13h for DL = 80h while a media 4 emulation is on: the frame as for
; cd_int13 (pushad, DS, ES; BP = SP + 4).
cd_hd_int13:
    push fs
    push gs
    xor ax, ax
    mov fs, ax
    push word EBDA_BASE >> 4
    pop gs
    mov ah, [bp + 29]
    cmp ah, 0x02
    je .read
    cmp ah, 0x08
    je .params
    cmp ah, 0x15
    je .type
    cmp ah, 0x41
    je .check
    cmp ah, 0x42
    je .ext_read
    cmp ah, 0x48
    je .ext_params
    cmp ah, 0x4B
    je .spec
    cmp ah, 0x03
    je .wrprot
    cmp ah, 0x43
    je .wrprot
    cmp ah, 0x00
    je .ok
    cmp ah, 0x01
    je .ok
    cmp ah, 0x04
    je .ok
    cmp ah, 0x0C
    je .ok
    cmp ah, 0x0D
    je .ok
    cmp ah, 0x10
    je .ok
    cmp ah, 0x11
    je .ok
    cmp ah, 0x44
    je .ok
.bad:
    mov byte [bp + 29], ST_BAD
    jmp .fail
.wrprot:
    mov byte [bp + 29], ST_WRPROT
    jmp .fail
.notfound:
    mov byte [bp + 29], ST_NOTFOUND
.fail:
    or word [bp + 36], 1
    jmp .out
.ok:
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
.out:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iret

.type:
    mov byte [bp + 29], 0x03            ; a hard disk
    mov eax, [gs:CD_HD_TOTAL]
    mov [bp + 20], ax                   ; DX = low word
    shr eax, 16
    mov [bp + 24], ax                   ; CX = high word
    and word [bp + 36], 0xFFFE
    jmp .out

.params:
    mov word [bp + 28], 0               ; AX = 0
    mov ax, [gs:CD_HD_CYLS]
    dec ax                              ; the last cylinder
    mov [bp + 25], al                   ; CH = its low 8 bits
    shl ah, 6
    or ah, [gs:CD_HD_SPT]
    mov [bp + 24], ah                   ; CL = sectors, cylinder bits 8-9
    mov al, [gs:CD_HD_HEADS]
    dec al
    mov [bp + 21], al                   ; DH = the last head
    mov byte [bp + 20], 1               ; DL = drives
    jmp .ok

.check:
    cmp word [bp + 16], 0x55AA
    jne .bad
    mov word [bp + 16], 0xAA55
    mov word [bp + 24], EDD_SUBSET_FIXED
    mov byte [bp + 29], EDD_VERSION
    and word [bp + 36], 0xFFFE
    jmp .out

.read:
    ; LBA = (cylinder * heads + head) * sectors + sector - 1
    movzx ecx, byte [bp + 28]            ; AL = count
    test cx, cx
    jz .bad
    mov byte [bp + 28], 0
    movzx eax, byte [bp + 24]
    and al, 0x3F
    jz .notfound
    cmp al, [gs:CD_HD_SPT]
    ja .notfound
    dec ax
    mov edi, eax                        ; EDI = sector - 1
    movzx eax, byte [bp + 24]
    shr al, 6
    mov ah, al
    mov al, [bp + 25]                   ; EAX = the cylinder
    cmp ax, [gs:CD_HD_CYLS]
    jae .notfound
    mov dl, [bp + 21]
    cmp dl, [gs:CD_HD_HEADS]
    jae .notfound
    movzx edx, byte [gs:CD_HD_HEADS]
    imul eax, edx
    movzx edx, byte [bp + 21]
    add eax, edx
    movzx edx, byte [gs:CD_HD_SPT]
    imul eax, edx
    add eax, edi                        ; EAX = the first LBA
    mov edx, eax
    add edx, ecx
    cmp edx, [gs:CD_HD_TOTAL]
    ja .notfound
    mov di, [bp + 16]                   ; ES:BX, the caller's ES
.sector:
    call .normal
    call cd_emu_sector
    jc .notfound
    add di, 512
    inc byte [bp + 28]
    inc eax
    loop .sector
    jmp .ok

; ES:DI normalised so that 512 bytes never wrap DI. Keeps every other
; register.
.normal:
    push ax
    push dx
    mov ax, di
    shr ax, 4
    mov dx, es
    add ax, dx
    mov es, ax
    and di, 0x000F
    pop dx
    pop ax
    ret

.ext_read:
    cmp byte [si], 0x10
    jb .bad
    movzx ecx, word [si + 2]
    mov word [si + 2], 0
    test cx, cx
    jz .ok
    cmp cx, 127
    ja .bad
    cmp dword [si + 12], 0
    jne .notfound
    mov eax, [si + 8]
    mov edx, eax
    add edx, ecx
    jc .notfound
    cmp edx, [gs:CD_HD_TOTAL]
    ja .notfound
    push es
    mov di, [si + 6]
    mov es, di
    mov di, [si + 4]
.ext_sector:
    call .normal
    call cd_emu_sector
    jc .ext_fail
    add di, 512
    inc word [si + 2]
    inc eax
    loop .ext_sector
    pop es
    jmp .ok
.ext_fail:
    pop es
    jmp .notfound

.ext_params:
    mov ax, [si]
    cmp ax, 0x1A
    jb .bad
    mov word [si + 2], 0x0002           ; flags: the CHS values are valid
    movzx eax, word [gs:CD_HD_CYLS]
    mov [si + 4], eax
    movzx eax, byte [gs:CD_HD_HEADS]
    mov [si + 8], eax
    movzx eax, byte [gs:CD_HD_SPT]
    mov [si + 12], eax
    mov eax, [gs:CD_HD_TOTAL]
    mov [si + 16], eax
    mov dword [si + 20], 0
    mov word [si + 24], 512
    mov cx, 0x1A
    cmp word [si], 0x1E
    jb .ext_size
    mov dword [si + 26], 0xFFFFFFFF     ; no DPTE
    mov cx, 0x1E
.ext_size:
    mov [si], cx
    jmp .ok

.spec:
    cmp byte [bp + 28], 0x01
    ja .bad
    cmp byte [bp + 28], 0
    jne .keep
    mov byte [gs:CD_EMUL], 0            ; terminate
.keep:
    mov byte [si], 0x13
    mov byte [si + 1], 4                ; media: hard disk
    mov byte [si + 2], CD_HD_DRIVE
    xor al, al
    cmp word [fs:CD_BASE], ATAPI_PRI
    je .ctrl
    inc al
.ctrl:
    mov [si + 3], al
    mov eax, [fs:CD_IMAGE_LBA]
    mov [si + 4], eax
    xor ax, ax
    cmp byte [fs:CD_SEL], 0xB0
    jne .dev
    inc ax
.dev:
    mov [si + 8], ax
    mov word [si + 10], 0
    mov ax, [fs:CD_LOAD_SEG]
    mov [si + 12], ax
    mov ax, [fs:CD_COUNT]
    mov [si + 14], ax
    mov ax, [gs:CD_HD_CYLS]
    dec ax
    shl ah, 6
    or ah, [gs:CD_HD_SPT]
    mov [si + 16], ah                   ; sectors, cylinder bits 8-9
    mov [si + 17], al                   ; the last cylinder's low bits
    mov al, [gs:CD_HD_HEADS]
    dec al
    mov [si + 18], al                   ; the last head
    jmp .ok
