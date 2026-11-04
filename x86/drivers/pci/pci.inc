; SquirrelBIOS x86 — PCI configuration access and POST enumeration
; Original code. Included by x86/squirrel-bios.asm when CONFIG_PCI.
;
; Configuration mechanism #1 (address at 0xCF8, data at 0xCFC-0xCFF), as
; the PCI Local Bus Specification 2.1 describes it. Bus 0 only: QEMU's
; i440fx and q35 machines put every default device there. Bridges are
; listed but nothing behind them is numbered or scanned yet.
;
; At POST, pci_init gives every memory BAR an address in
; PCI_MEM_BASE-PCI_MEM_LIMIT and every I/O BAR a port in
; PCI_IO_BASE-0xFFFF, each aligned to its size, then turns on memory and
; I/O decoding for the functions that got one (a decode bit already on
; stays on). 64-bit BARs are placed
; below 4 GB. Expansion ROM BARs are left off. On i440fx it routes the
; PIIX3's PIRQA-D to IRQ 10, 10, 11, 11 (level-triggered) and writes each
; function's Interrupt Line to match; elsewhere Interrupt Line is 0xFF.
;
; Access routines, BH = bus, BL = device << 3 | function, DI = register:
;   pci_read8/16/32    EAX = the register, zero-extended
;   pci_write8/16/32   CL / CX / ECX = the value
; All of them keep every register but EAX (reads) and the flags.

PCI_ADDR        equ 0xCF8
PCI_DATA        equ 0xCFC
PCI_ENABLE      equ 0x80000000

PCI_VENDOR      equ 0x00
PCI_CMD         equ 0x04
PCI_HEADER      equ 0x0E
PCI_BAR0        equ 0x10
PCI_INT_LINE    equ 0x3C
PCI_INT_PIN     equ 0x3D

PCI_CMD_IO      equ 0x01
PCI_CMD_MEM     equ 0x02
PCI_CMD_MASTER  equ 0x04

PCI_MEM_BASE    equ 0xE0000000  ; above the RAM QEMU maps below 4 GB
PCI_MEM_LIMIT   equ 0xFEC00000  ; IOAPIC, HPET, LAPIC and the ROM above
PCI_IO_BASE     equ 0xC000

; PIIX3 (i440fx) PCI-to-ISA bridge at 00:01.0: the interrupt router
PIIX3_ID        equ 0x70008086
PIIX3_BDF       equ 0x0008
PIIX3_PIRQ      equ 0x60        ; PIRQA-D route registers 0x60-0x63
ELCR2           equ 0x4D1       ; edge/level control, IRQ 8-15
PCI_IRQ_A       equ 10          ; PIRQA and PIRQB
PCI_IRQ_C       equ 11          ; PIRQC and PIRQD

; Point 0xCF8 at BX / DI. Out: DX = the data port for that register.
; Clobbers EAX.
pci_select:
    movzx eax, bx
    shl eax, 8
    mov dx, di
    and dl, 0xFC
    mov al, dl
    or eax, PCI_ENABLE
    mov dx, PCI_ADDR
    out dx, eax
    mov dx, di
    and dx, 3
    add dx, PCI_DATA
    ret

pci_read8:
    push dx
    call pci_select
    in al, dx
    movzx eax, al
    pop dx
    ret

pci_read16:
    push dx
    call pci_select
    in ax, dx
    movzx eax, ax
    pop dx
    ret

pci_read32:
    push dx
    call pci_select
    in eax, dx
    pop dx
    ret

pci_write8:
    push eax
    push dx
    call pci_select
    mov al, cl
    out dx, al
    pop dx
    pop eax
    ret

pci_write16:
    push eax
    push dx
    call pci_select
    mov ax, cx
    out dx, ax
    pop dx
    pop eax
    ret

pci_write32:
    push eax
    push dx
    call pci_select
    mov eax, ecx
    out dx, eax
    pop dx
    pop eax
    ret

; CF clear if configuration mechanism #1 answers. Keeps every register
; but the flags.
pci_present:
    push eax
    push dx
    mov dx, PCI_ADDR
    in eax, dx
    push eax
    mov eax, PCI_ENABLE
    out dx, eax
    in eax, dx
    cmp eax, PCI_ENABLE
    pop eax
    out dx, eax                 ; put the old address back
    pop dx
    pop eax
    je .yes
    stc
    ret
.yes:
    clc
    ret

; BX = the first bus 0 function to look at. Out: CF clear and BX = the
; first function present at or after it; CF set when bus 0 has no more.
; Functions 1-7 count only when function 0 says multi-function.
; Clobbers EAX and DI.
pci_next:
    test bl, 7
    jz .probe
    push bx
    and bl, 0xF8
    mov di, PCI_HEADER
    call pci_read8
    pop bx
    test al, 0x80
    jnz .probe
    or bl, 7                    ; single function: skip the rest
    jmp .advance
.probe:
    mov di, PCI_VENDOR
    call pci_read16
    cmp ax, 0xFFFF
    jne .found
.advance:
    inc bl
    jnz pci_next
    stc
    ret
.found:
    clc
    ret

; PIIX3 at 00:01.0: route PIRQA-D and make IRQ 10 / 11 level-triggered.
; CF clear if the router is there. Keeps every register but the flags.
pci_route_init:
    pushad
    mov bx, PIIX3_BDF
    mov di, PCI_VENDOR
    call pci_read32
    cmp eax, PIIX3_ID
    jne .none
    mov di, PIIX3_PIRQ
    mov cl, PCI_IRQ_A
    call pci_write8             ; PIRQA
    inc di
    call pci_write8             ; PIRQB
    inc di
    mov cl, PCI_IRQ_C
    call pci_write8             ; PIRQC
    inc di
    call pci_write8             ; PIRQD
    mov dx, ELCR2
    in al, dx
    or al, (1 << (PCI_IRQ_A - 8)) | (1 << (PCI_IRQ_C - 8))
    out dx, al
    popad
    clc
    ret
.none:
    popad
    stc
    ret

; POST: size and place every BAR on bus 0, enable decoding, set the
; Interrupt Line. Keeps every register.
pci_init:
    pushad
    mov bp, sp
    sub sp, 12
    ; [bp-4]  next memory address    [bp-6]  next I/O port
    ; [bp-7]  decode bits to enable  [bp-8]  register after the last BAR
    ; [bp-9]  this BAR is 64-bit     [bp-10] PIIX3 router found
    ; [bp-12] the function's command register before sizing
    call pci_present
    jc .out
    mov byte [bp - 10], 0
    call pci_route_init
    jc .no_router
    mov byte [bp - 10], 1
.no_router:
    mov dword [bp - 4], PCI_MEM_BASE
    mov word [bp - 6], PCI_IO_BASE
    xor bx, bx
.fn:
    call pci_next
    jc .out
    mov di, PCI_HEADER
    call pci_read8
    and al, 0x7F
    mov byte [bp - 8], PCI_BAR0 + 6 * 4
    test al, al
    jz .bars
    mov byte [bp - 8], PCI_BAR0 + 2 * 4
    cmp al, 1                   ; PCI-to-PCI bridge: two BARs
    jne .irq                    ; CardBus and others: left alone
.bars:
    mov byte [bp - 7], 0
    mov di, PCI_CMD
    call pci_read16
    mov [bp - 12], ax
    mov cx, ax
    and cl, ~(PCI_CMD_IO | PCI_CMD_MEM) & 0xFF
    call pci_write16            ; no decoding while the BARs move
    mov di, PCI_BAR0
.bar:
    mov byte [bp - 9], 0
    mov ecx, 0xFFFFFFFF
    call pci_write32
    call pci_read32             ; EAX = what the BAR lets through
    test eax, eax
    jz .next_bar                ; not implemented (it reads back 0)
    test al, 1
    jnz .io
    mov edx, eax
    and edx, 0xFFFFFFF0         ; -size
    and al, 0x06
    cmp al, 0x04
    jne .mem
    mov byte [bp - 9], 1        ; 64-bit: the upper half goes to 0
    add di, 4
    xor ecx, ecx
    call pci_write32
    sub di, 4
    test edx, edx
    jz .unplaced                ; 4 GB or more: no room below 4 GB
.mem:
    mov ecx, edx
    neg ecx                     ; size
    mov eax, [bp - 4]
    call pci_place
    jc .unplaced
    cmp eax, PCI_MEM_LIMIT
    ja .unplaced
    mov [bp - 4], eax
    or byte [bp - 7], PCI_CMD_MEM
    jmp .next_bar
.io:
    mov edx, eax
    or edx, 0xFFFF0003          ; 16-bit decoders read 0 above bit 15
    xor dl, 3
    mov ecx, edx
    neg ecx
    movzx eax, word [bp - 6]
    call pci_place
    jc .unplaced
    cmp eax, 0x10000
    ja .unplaced
    mov [bp - 6], ax            ; 0x10000 wraps to 0: the pool is used up
    test ax, ax
    jnz .io_ok
    dec word [bp - 6]           ; keep it at 0xFFFF so nothing else fits
.io_ok:
    or byte [bp - 7], PCI_CMD_IO
    jmp .next_bar
.unplaced:
    xor ecx, ecx
    call pci_write32
.next_bar:
    add di, 4
    cmp byte [bp - 9], 0
    je .bar_end
    add di, 4
.bar_end:
    mov ax, di
    cmp al, [bp - 8]
    jb .bar
    mov di, PCI_CMD
    mov cx, [bp - 12]           ; as it was, plus what the BARs need
    or cl, [bp - 7]
    call pci_write16

.irq:
    mov di, PCI_INT_PIN
    call pci_read8
    test al, al
    jz .next_fn
    mov cl, 0xFF                ; unknown
    cmp byte [bp - 10], 0
    je .line
    ; PIIX3: device d's INTx (1-4) reaches PIRQ (d - 1 + x - 1) mod 4
    mov cl, bl
    shr cl, 3
    add cl, al
    sub cl, 2
    and cl, 3
    mov al, PCI_IRQ_A
    cmp cl, 2
    jb .irq_set
    mov al, PCI_IRQ_C
.irq_set:
    mov cl, al
.line:
    mov di, PCI_INT_LINE
    call pci_write8
.next_fn:
    inc bl
    jnz .fn
.out:
    mov sp, bp
    popad
    ret

; Place a BAR of ECX bytes (a power of two), EDX = -ECX, at or above
; EAX, then write the address to the BAR at BX / DI.
; Out: CF clear and EAX = the first byte after it; CF set if it would
; run past 4 GB (nothing is written). Keeps ECX and EDX.
pci_place:
    push ecx
    dec ecx
    add eax, ecx
    jc .over
    and eax, edx                ; aligned to the size
    pop ecx
    push ecx
    mov ecx, eax
    call pci_write32
    pop ecx
    add eax, ecx
    jc .over_popped
    clc
    ret
.over:
    pop ecx
.over_popped:
    stc
    ret
