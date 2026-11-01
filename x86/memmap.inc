; SquirrelBIOS — x86 memory map hand-off
; Original code.
;
; Builds an E820-style table at 0x0000:0x0500 from the RAM sizes QEMU
; leaves in CMOS. Layout is documented in common/handoff.md.
;
;   0x0500  dd  'SQMM'          magic
;   0x0504  dw  count           number of entries
;   0x0506  dw  20              bytes per entry
;   0x0508  entries: dq base, dq length, dd type

MEMMAP_ADDR     equ 0x0500
MEMMAP_ENTRIES  equ MEMMAP_ADDR + 8
MEMMAP_ENTRY    equ 20

E820_USABLE     equ 1
E820_RESERVED   equ 2

; CMOS registers QEMU fills in (units in the comments)
CMOS_EXT_LO     equ 0x30        ; KB above 1 MB, capped at 0xFFFF
CMOS_EXT_HI     equ 0x31
CMOS_EXT16_LO   equ 0x34        ; 64 KB blocks above 16 MB, below 4 GB
CMOS_EXT16_HI   equ 0x35
CMOS_HIGH_0     equ 0x5B        ; 64 KB blocks above 4 GB, 24 bits
CMOS_HIGH_1     equ 0x5C
CMOS_HIGH_2     equ 0x5D

; Fill in the table. Needs a stack; preserves ES and DI.
build_memmap:
    push es
    push di
    xor ax, ax
    mov es, ax
    mov dword [es:MEMMAP_ADDR], 'SQMM'
    mov word [es:MEMMAP_ADDR + 4], 0
    mov word [es:MEMMAP_ADDR + 6], MEMMAP_ENTRY
    mov di, MEMMAP_ENTRIES

    ; Conventional memory
    xor eax, eax
    xor edx, edx
    mov ecx, 0xA0000
    xor esi, esi
    mov bl, E820_USABLE
    call memmap_add

    ; VGA window, option ROMs, this firmware
    mov eax, 0xA0000
    mov ecx, 0x60000
    mov bl, E820_RESERVED
    call memmap_add

    ; Extended memory up to the top of RAM below 4 GB
    mov ah, CMOS_EXT16_HI
    mov al, CMOS_EXT16_LO
    call cmos_read16
    test eax, eax
    jz .small
    shl eax, 16
    add eax, 0x1000000
    jmp .low_top
.small:
    mov ah, CMOS_EXT_HI
    mov al, CMOS_EXT_LO
    call cmos_read16
    shl eax, 10
    add eax, 0x100000
.low_top:
    mov ecx, eax
    sub ecx, 0x100000
    jz .no_ext
    mov eax, 0x100000
    xor edx, edx
    xor esi, esi
    mov bl, E820_USABLE
    call memmap_add
.no_ext:

    ; IOAPIC, LAPIC and the firmware alias below 4 GB
    mov eax, 0xFEC00000
    xor edx, edx
    mov ecx, 0x01400000
    xor esi, esi
    mov bl, E820_RESERVED
    call memmap_add

    ; RAM above 4 GB
    mov ah, CMOS_HIGH_1
    mov al, CMOS_HIGH_0
    call cmos_read16
    mov ecx, eax                ; bits 15:0 of the block count
    mov al, CMOS_HIGH_2
    call cmos_read
    movzx esi, al               ; bits 23:16
    mov eax, ecx
    or eax, esi
    jz .done
    shl ecx, 16                 ; length = blocks * 64 KB
    xor eax, eax
    mov edx, 1                  ; base = 4 GB
    mov bl, E820_USABLE
    call memmap_add
.done:
    pop di
    pop es
    ret

; Append one entry.
; In: EDX:EAX = base, ESI:ECX = length, BL = type, ES:DI = next slot.
; Out: DI advanced, count incremented. Clobbers EBX.
memmap_add:
    mov [es:di], eax
    mov [es:di + 4], edx
    mov [es:di + 8], ecx
    mov [es:di + 12], esi
    movzx ebx, bl
    mov [es:di + 16], ebx
    add di, MEMMAP_ENTRY
    inc word [es:MEMMAP_ADDR + 4]
    ret

; In: AL = CMOS register.  Out: AL = value.
; Bit 7 of port 0x70 masks NMI, so the index is written with it set and
; NMI stays off while the register is selected and read. Afterwards the
; index goes back to status register D (read-only) with NMI enabled, which
; is how the OS finds it.
cmos_read:
    or al, 0x80
    out 0x70, al
    in al, 0x71
    push ax
    mov al, 0x0D
    out 0x70, al
    pop ax
    ret

; In: AH = high register, AL = low register.  Out: EAX = 16-bit value.
cmos_read16:
    push cx
    mov cl, ah
    call cmos_read
    xchg al, cl                 ; CL = low byte, AL = high register
    call cmos_read
    mov ah, al
    mov al, cl
    movzx eax, ax
    pop cx
    ret
