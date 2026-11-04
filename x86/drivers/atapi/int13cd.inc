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
