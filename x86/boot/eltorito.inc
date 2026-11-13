; SquirrelBIOS x86 — El Torito no-emulation boot from an ATAPI CD/DVD
; Original code. Included by x86/squirrel-bios.asm when CONFIG_CDBOOT.
;
; From the "El Torito" Bootable CD-ROM Format Specification 1.0: the Boot
; Record Volume Descriptor in block 17 names the boot catalog; its
; validation entry (header 01h, key 55h AAh, words summing to 0) is
; followed by the initial/default entry (88h = bootable, media type,
; load segment, sector count in 512-byte units, image block). A
; no-emulation image is entered at load segment:0 with DL = CD_DRIVE, and
; INT 13h then reads the CD in 2048-byte blocks under that number. A 1.2,
; 1.44 or 2.88 MB diskette image (media 1-3) becomes drive 00h instead:
; its first sectors are loaded and entered with DL = 00h, and INT 13h
; reads the image as a write-protected diskette
; (drivers/atapi/int13cd.inc). Hard disk emulation is not supported.

CD_DRIVE        equ 0xE0        ; INT 13h drive number of the booted CD
CD_SCRATCH      equ 0x8000      ; volume descriptor and catalog, at POST
CD_LOAD_MIN     equ BOOT_SECTOR ; no image below 0x7C00
ELT_BRVD        equ 17          ; block of the Boot Record Volume Descriptor

; Called when no hard disk booted. Tries every ATAPI unit on both IDE
; channels; a bootable no-emulation image is loaded and entered (does
; not return). Otherwise each unit that was tried gets a line saying
; why, and it returns with SI, BX, DS and ES as they were and DI moved
; past the lines it printed (the console position).
cd_boot:
    push fs
    push eax
    push ecx
    push edx
    push ebp
    push si
    push bx
    xor ax, ax
    mov fs, ax
    mov si, cd_units
.unit:
    mov ax, [cs:si]
    test ax, ax
    jz .none
    mov [fs:CD_BASE], ax
    mov al, [cs:si + 2]
    mov [fs:CD_SEL], al
    add si, 3
    call atapi_probe
    jc .unit
    push si
    call cd_try                 ; returns only with SI = why not
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    pop si
    jmp .unit
.none:
    mov byte [fs:CD_SEL], 0     ; nothing booted from a CD
    pop bx
    pop si
    pop ebp
    pop edx
    pop ecx
    pop eax
    pop fs
    ret

; IDE units in the order they are tried: base port, device select.
cd_units:
    dw ATAPI_PRI
    db 0xA0
    dw ATAPI_PRI
    db 0xB0
    dw ATAPI_SEC
    db 0xA0
    dw ATAPI_SEC
    db 0xB0
    dw 0

; Boot the CD at CD_BASE / CD_SEL, or return SI = the reason it cannot.
cd_try:
    mov si, msg_cdread
    mov eax, ELT_BRVD
    mov cx, 1
    mov ebp, CD_SCRATCH
    call atapi_read
    jc .ret
    mov si, msg_cdnoboot
    cmp byte [fs:CD_SCRATCH], 0             ; boot record
    jne .ret
    cmp dword [fs:CD_SCRATCH + 1], 'CD00'
    jne .ret
    cmp byte [fs:CD_SCRATCH + 5], '1'
    jne .ret
    push si
    push di
    push es
    push ds
    push fs
    pop es
    push cs
    pop ds
    mov si, elt_ident
    mov di, CD_SCRATCH + 7
    mov cx, elt_ident_len
    repe cmpsb
    pop ds
    pop es
    pop di
    pop si
    jne .ret

    mov eax, [fs:CD_SCRATCH + 0x47]         ; boot catalog block
    mov cx, 1
    call atapi_read
    mov si, msg_cdread
    jc .ret
    mov si, msg_cdcatalog
    cmp byte [fs:CD_SCRATCH], 1             ; validation entry
    jne .ret
    cmp word [fs:CD_SCRATCH + 0x1E], 0xAA55
    jne .ret
    xor ax, ax
    mov bx, CD_SCRATCH
.sum:
    add ax, [fs:bx]
    add bx, 2
    cmp bx, CD_SCRATCH + 0x20
    jb .sum
    test ax, ax
    jnz .ret
    mov si, msg_cdnotboot
    cmp byte [fs:CD_SCRATCH + 0x20], 0x88   ; bootable
    jne .ret
    mov si, msg_cdemul
    mov al, [fs:CD_SCRATCH + 0x21]          ; media: 0 = no emulation,
    and al, 0x0F                            ; 1-3 = diskettes
    cmp al, 3
    ja .ret

    push es
    push word EBDA_BASE >> 4
    pop es
    mov al, [fs:CD_SCRATCH + 0x21]
    and al, 0x0F
    mov [es:CD_EMUL], al                    ; 0 = no emulation
    pop es
    mov ax, [fs:CD_SCRATCH + 0x22]          ; load segment, 0 = 07C0h
    test ax, ax
    jnz .seg
    mov ax, BOOT_SECTOR >> 4
.seg:
    mov [fs:CD_LOAD_SEG], ax
    mov ax, [fs:CD_SCRATCH + 0x26]          ; 512-byte sectors
    mov [fs:CD_COUNT], ax
    mov eax, [fs:CD_SCRATCH + 0x28]
    mov [fs:CD_IMAGE_LBA], eax

    ; Whole blocks are read, so the room checked is rounded up to them
    mov si, msg_cdrange
    movzx ecx, word [fs:CD_COUNT]
    test cx, cx
    jz .ret
    add ecx, 3
    shr ecx, 2                              ; blocks
    movzx ebp, word [fs:CD_LOAD_SEG]
    shl ebp, 4
    cmp ebp, CD_LOAD_MIN
    jb .ret
    mov edx, ecx
    shl edx, 11
    add edx, ebp
    cmp edx, LOAD_LIMIT
    ja .ret
    push es
    push word EBDA_BASE >> 4
    pop es
    cmp byte [es:CD_EMUL], 0
    pop es
    jne .emulated
    call atapi_read                         ; EAX = image, CX = blocks
    mov si, msg_cdread
    jc .ret

    mov byte [ss:BOOT_DL], CD_DRIVE
    mov si, msg_cdboot
    jmp .go
.emulated:
    ; The first CD_COUNT sectors of the diskette, to the load segment
    push es
    push di
    mov ax, [fs:CD_LOAD_SEG]
    mov es, ax
    xor di, di
    xor eax, eax
    mov cx, [fs:CD_COUNT]
.emu_sector:
    call cd_emu_sector
    jc .emu_fail
    inc eax
    mov dx, es
    add dx, 512 >> 4
    mov es, dx
    loop .emu_sector
    pop di
    pop es
    mov byte [ss:BOOT_DL], CD_EMUL_DRIVE
    or byte [ss:BDA_EQUIP], 1               ; a diskette drive is there
    mov si, msg_cdemu_boot
.go:
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    jmp cd_handoff
.ret:
    ret
.emu_fail:
    pop di
    pop es
    mov si, msg_cdread
    ret

elt_ident       db 'EL TORITO SPECIFICATION'
elt_ident_len   equ $ - elt_ident

; As handoff, but the image is entered at CD_LOAD_SEG:0.
cd_handoff:
    call prepare_runtime
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
    mov dl, [BOOT_DL]
    mov di, MEMMAP_ADDR
    push word [CD_LOAD_SEG]
    push word 0
    retf
