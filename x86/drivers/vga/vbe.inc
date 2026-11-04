; SquirrelBIOS x86 — VBE 3.0 linear-framebuffer modes (INT 10h AH=4Fh)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_VBE.
;
; Written from the VESA BIOS Extension Core Functions Standard 3.0 and
; the Bochs/QEMU display interface (the "DISPI" registers at ports
; 1CEh/1CFh) that QEMU's standard VGA (PCI 1234:1111) has. The modes are
; 16- and 32-bit direct colour, reached only through the linear frame
; buffer (the VGA function's BAR0); there are no banked windows. Mode 3
; (BX = 0003h) returns to the 80x25 text mode that POST set up.
;
; Functions: 4F00h controller information, 4F01h mode information,
; 4F02h set mode, 4F03h current mode, 4F06h scan line length (get only),
; 4F07h display start. Others answer AX = 0100h (not supported).

VBE_INDEX       equ 0x01CE
VBE_DATA        equ 0x01CF
DISPI_ID        equ 0
DISPI_XRES      equ 1
DISPI_YRES      equ 2
DISPI_BPP       equ 3
DISPI_ENABLE    equ 4
DISPI_VWIDTH    equ 6
DISPI_VHEIGHT   equ 7
DISPI_XOFF      equ 8
DISPI_YOFF      equ 9
DISPI_VRAM64K   equ 0x0A
DISPI_ON        equ 0x01
DISPI_LFB       equ 0x40
DISPI_NOCLEAR   equ 0x80
VBE_VGA_ID      equ 0x11111234      ; QEMU's standard VGA: device, vendor

VBE_MODE        equ 0x00E0          ; EBDA word: current mode, as 4F03h
                                    ; reports it (0: text mode 3)

; The modes: number, width, height, bits per pixel
vbe_modes:
    dw 0x0111, 640, 480, 16
    dw 0x0112, 640, 480, 32
    dw 0x0114, 800, 600, 16
    dw 0x0115, 800, 600, 32
    dw 0x0117, 1024, 768, 16
    dw 0x0118, 1024, 768, 32
    dw 0x011A, 1280, 1024, 16
    dw 0x011B, 1280, 1024, 32
vbe_modes_end:
VBE_NMODES      equ (vbe_modes_end - vbe_modes) / 8

vbe_oem         db 'SquirrelBIOS', 0
vbe_vendor      db 'Squirrelworks', 0
vbe_product     db 'QEMU standard VGA', 0
vbe_rev         db '1', 0

; INT 10h AH=4Fh, from int10_handler with its frame at BP (the caller's
; flags at [bp+6]). Returns with IRET.
vbe_handler:
    cld
    call vbe_present
    jc .none
    cmp al, 0x00
    je vbe_info
    cmp al, 0x01
    je vbe_mode_info
    cmp al, 0x02
    je vbe_set_mode
    cmp al, 0x03
    je vbe_get_mode
    cmp al, 0x06
    je vbe_scanline
    cmp al, 0x07
    je vbe_start
.none:
    mov ax, 0x0100
    pop bp
    iret

; Return from vbe_handler: AX = 004Fh (success) or 014Fh (failed).
vbe_ok:
    mov ax, 0x004F
    pop bp
    iret
vbe_fail:
    mov ax, 0x014F
    pop bp
    iret

; CF clear if the DISPI interface answers. Keeps every register.
vbe_present:
    push ax
    mov al, DISPI_ID
    call vbe_read
    and ax, 0xFFF0
    cmp ax, 0xB0C0
    pop ax
    je .yes
    stc
    ret
.yes:
    clc
    ret

; DISPI register AL: AX = its value. Keeps every other register.
vbe_read:
    push dx
    mov dx, VBE_INDEX
    xor ah, ah
    out dx, ax
    inc dx
    in ax, dx
    pop dx
    ret

; DISPI register AL = BX. Keeps every register.
vbe_write:
    push ax
    push dx
    mov dx, VBE_INDEX
    xor ah, ah
    out dx, ax
    inc dx
    mov ax, bx
    out dx, ax
    pop dx
    pop ax
    ret

; The mode in CX (bits 8-0): SI = its entry in vbe_modes. CF set if it is
; not one of them. Keeps every other register.
vbe_find:
    push ax
    mov ax, cx
    and ax, 0x01FF
    mov si, vbe_modes
.look:
    cmp [cs:si], ax
    je .found
    add si, 8
    cmp si, vbe_modes_end
    jb .look
    pop ax
    stc
    ret
.found:
    pop ax
    clc
    ret

; EAX = the frame buffer's address: BAR0 of the first PCI function that
; is QEMU's standard VGA (0 if there is none). Keeps every other register.
vbe_lfb:
    push bx
    push di
    call pci_present
    jc .none
    xor bx, bx
.fn:
    call pci_next
    jc .none
    xor di, di
    call pci_read32
    cmp eax, VBE_VGA_ID
    je .found
    inc bl
    jnz .fn
.none:
    xor eax, eax
    pop di
    pop bx
    ret
.found:
    mov di, PCI_BAR0
    call pci_read32
    and eax, 0xFFFFFFF0
    pop di
    pop bx
    ret

; 4F00h: the controller information block at ES:DI (512 bytes when it
; starts with 'VBE2', else 256). The mode list and the strings go in
; the block itself.
vbe_info:
    push eax
    push bx
    push cx
    push si
    push di
    mov bx, di                          ; BX = the block
    mov cx, 256 / 2
    cmp dword [es:di], 'VBE2'
    jne .clear
    mov cx, 512 / 2
.clear:
    push cx
    xor ax, ax
    rep stosw
    pop cx
    mov dword [es:bx], 'VESA'
    mov word [es:bx + 4], 0x0300        ; version 3.0
    mov dword [es:bx + 10], 0           ; capabilities: 6-bit DAC, VGA
    mov word [es:bx + 0x14], 0x0001     ; OEM software revision
    mov al, DISPI_VRAM64K
    call vbe_read
    test ax, ax
    jnz .vram
    mov ax, 0x0100                      ; 16 MB, if it does not say
.vram:
    mov [es:bx + 0x12], ax              ; total memory, 64 KB units
    ; The mode list, in the reserved area at +22h
    lea di, [bx + 0x22]
    mov [es:bx + 0x0E], di
    mov [es:bx + 0x10], es
    mov si, vbe_modes
.mode:
    mov ax, [cs:si]
    stosw
    add si, 8
    cmp si, vbe_modes_end
    jb .mode
    mov ax, 0xFFFF
    stosw
    ; The strings: in the OEM data area at +100h of a VBE 2.0 block,
    ; else in the reserved area after the mode list
    cmp cx, 512 / 2
    jne .strings
    lea di, [bx + 0x100]
.strings:
    mov si, vbe_oem
    lea ax, [bx + 0x06]
    call vbe_string
    mov si, vbe_vendor
    lea ax, [bx + 0x16]
    call vbe_string
    mov si, vbe_product
    lea ax, [bx + 0x1A]
    call vbe_string
    mov si, vbe_rev
    lea ax, [bx + 0x1E]
    call vbe_string
    pop di
    pop si
    pop cx
    pop bx
    pop eax
    jmp vbe_ok

; Copy the string at CS:SI to ES:DI and store its far pointer at ES:AX.
; DI moves past it. Keeps every other register.
vbe_string:
    push si
    push bx
    mov bx, ax
    mov [es:bx], di
    mov [es:bx + 2], es
.c:
    mov al, [cs:si]
    stosb
    inc si
    test al, al
    jnz .c
    pop bx
    pop si
    ret

; 4F01h: the mode information block for mode CX at ES:DI (256 bytes).
vbe_mode_info:
    push eax
    push bx
    push cx
    push dx
    push si
    push di
    call vbe_find
    jc .fail
    push cx
    mov bx, di
    mov cx, 256 / 2
    xor ax, ax
    rep stosw
    pop cx
    ; Supported, extended information, colour, graphics, not VGA,
    ; no windows, linear frame buffer
    mov word [es:bx], 0x00FB
    mov ax, [cs:si + 2]
    mov [es:bx + 18], ax                ; width
    mov dx, [cs:si + 6]                 ; bits per pixel
    shr dx, 3
    mul dx                              ; AX = bytes per line
    mov [es:bx + 16], ax
    mov [es:bx + 50], ax                ; and in the linear mode
    mov cx, ax
    mov ax, [cs:si + 4]
    mov [es:bx + 20], ax                ; height
    mov byte [es:bx + 22], 8            ; character cell
    mov byte [es:bx + 23], 16
    mov byte [es:bx + 24], 1            ; planes
    mov al, [cs:si + 6]
    mov [es:bx + 25], al                ; bits per pixel
    mov byte [es:bx + 26], 1            ; banks
    mov byte [es:bx + 27], 6            ; direct colour
    mov byte [es:bx + 30], 1
    ; Image pages: how many screens the video memory holds, less one
    movzx eax, cx
    movzx edx, word [cs:si + 4]
    imul eax, edx
    mov ecx, eax                        ; ECX = bytes per screen
    mov al, DISPI_VRAM64K
    call vbe_read
    test ax, ax
    jnz .pages
    mov ax, 0x0100
.pages:
    movzx eax, ax
    shl eax, 16                         ; EAX = video memory
    xor edx, edx
    div ecx
    dec eax
    cmp eax, 255
    jbe .count
    mov al, 255
.count:
    mov [es:bx + 29], al
    mov [es:bx + 52], al
    mov [es:bx + 53], al
    ; Colour fields: R, G, B, reserved (size, position)
    mov eax, 0x050B0605                 ; 16: R 5 at 11, G 6 at 5
    mov edx, 0x00000005                 ;     B 5 at 0
    cmp byte [cs:si + 6], 32
    jne .fields
    mov eax, 0x08080810                 ; 32: R 8 at 16, G 8 at 8
    mov edx, 0x18080008                 ;     B 8 at 0, X 8 at 24
.fields:
    xchg al, ah                         ; R size, then R position
    mov [es:bx + 31], ax
    mov [es:bx + 54], ax
    shr eax, 16
    xchg al, ah
    mov [es:bx + 33], ax
    mov [es:bx + 56], ax
    mov [es:bx + 35], dl                ; B size
    mov [es:bx + 58], dl
    mov byte [es:bx + 36], 0            ; B position
    mov byte [es:bx + 59], 0
    shr edx, 16
    xchg dl, dh
    mov [es:bx + 37], dx                ; reserved size, position
    mov [es:bx + 60], dx
    call vbe_lfb
    mov [es:bx + 40], eax
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    jmp vbe_ok
.fail:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    jmp vbe_fail

; 4F02h: set mode BX (bit 14 linear frame buffer, which these modes
; need; bit 15 keep the video memory). BX = 3 returns to text mode.
vbe_set_mode:
    push bx
    push cx
    push si
    mov cx, bx
    and cx, 0x01FF
    cmp cx, 3
    je .text
    call vbe_find
    jc .fail
    test bx, 0x4000
    jz .fail                            ; no banked windows
    push bx
    xor bx, bx
    mov al, DISPI_ENABLE
    call vbe_write
    mov bx, [cs:si + 2]
    mov al, DISPI_XRES
    call vbe_write
    mov al, DISPI_VWIDTH
    call vbe_write
    mov bx, [cs:si + 4]
    mov al, DISPI_YRES
    call vbe_write
    mov bx, [cs:si + 6]
    mov al, DISPI_BPP
    call vbe_write
    xor bx, bx
    mov al, DISPI_XOFF
    call vbe_write
    mov al, DISPI_YOFF
    call vbe_write
    pop bx
    mov cx, bx
    and cx, 0x41FF                      ; what 4F03h reports
    mov ax, bx
    mov bx, DISPI_ON | DISPI_LFB
    test ax, 0x8000
    jz .enable
    or bx, DISPI_NOCLEAR
.enable:
    mov al, DISPI_ENABLE
    call vbe_write
    jmp .set
.text:
    ; Back to 80x25 text, as POST left it
    xor bx, bx
    mov al, DISPI_ENABLE
    call vbe_write
    pusha
    push ds
    push es
    push cs
    pop ds
    call vga_init
    pop es
    pop ds
    popa
    push ds
    push word 0
    pop ds
    mov word [BDA_CURSOR], 0
    pop ds
    call tty_hwcursor
    xor cx, cx
.set:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov [es:VBE_MODE], cx
    pop es
    pop si
    pop cx
    pop bx
    jmp vbe_ok
.fail:
    pop si
    pop cx
    pop bx
    jmp vbe_fail

; 4F03h: BX = the current mode.
vbe_get_mode:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov bx, [es:VBE_MODE]
    pop es
    test bx, bx
    jnz vbe_ok
    mov bx, 3
    jmp vbe_ok

; 4F06h: BL = 01h get the logical scan line: BX = bytes, CX = pixels,
; DX = lines the video memory holds. Setting it is not supported.
vbe_scanline:
    cmp bl, 1
    jne vbe_fail
    call vbe_cur
    jc vbe_fail
    push eax
    mov cx, [cs:si + 2]
    mov ax, [cs:si + 6]
    shr ax, 3
    mul cx
    mov bx, ax                          ; bytes per line
    push bx
    mov al, DISPI_VRAM64K
    call vbe_read
    movzx eax, ax
    shl eax, 16
    pop bx
    movzx ebx, bx
    xor edx, edx
    div ebx
    mov dx, ax                          ; lines
    pop eax
    jmp vbe_ok

; 4F07h: BL = 00h/80h set the display start to pixel CX of line DX;
; BL = 01h get it into CX, DX.
vbe_start:
    call vbe_cur
    jc vbe_fail
    cmp bl, 1
    je .get
    test bl, 0x7F
    jnz vbe_fail
    push ax
    push bx
    mov bx, cx
    mov al, DISPI_XOFF
    call vbe_write
    mov bx, dx
    mov al, DISPI_YOFF
    call vbe_write
    pop bx
    pop ax
    jmp vbe_ok
.get:
    push ax
    mov al, DISPI_XOFF
    call vbe_read
    mov cx, ax
    mov al, DISPI_YOFF
    call vbe_read
    mov dx, ax
    pop ax
    xor bh, bh
    jmp vbe_ok

; SI = the current mode's entry; CF set in text mode. Keeps every other
; register.
vbe_cur:
    push cx
    push es
    push word EBDA_BASE >> 4
    pop es
    mov cx, [es:VBE_MODE]
    pop es
    jcxz .text
    call vbe_find
    pop cx
    ret
.text:
    pop cx
    stc
    ret
