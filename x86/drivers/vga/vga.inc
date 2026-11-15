; SquirrelBIOS — VGA set-up: 80x25 colour text at 0xB8000 with our own font
; Original code. The register values are the standard VGA ones for an
; 80x25 text mode with 9x16 character cells (720x400, 70 Hz).
;
; Nothing here uses a BIOS: the firmware runs no option ROMs, so on QEMU
; the VGA starts unprogrammed and blank.

VGA_MISC_W      equ 0x3C2
VGA_SEQ_I       equ 0x3C4
VGA_GC_I        equ 0x3CE
VGA_CRTC_I      equ 0x3D4
VGA_AC          equ 0x3C0
VGA_IS1         equ 0x3DA           ; read: resets the attribute flip-flop
VGA_DAC_W       equ 0x3C8
VGA_DAC_DATA    equ 0x3C9

VGA_COLS        equ 80
VGA_ROWS        equ 25

; In: nothing (DS = ROM segment).  Clobbers AX, BX, CX, DX, SI, DI.
; Leaves ES as it found it.
vga_init:
    push es

    ; Sequencer held in synchronous reset while the clock changes
    mov dx, VGA_SEQ_I
    mov ax, 0x0100
    out dx, ax

    ; Misc output: colour I/O at 0x3Dx, RAM enabled, 28 MHz clock, 400 lines
    mov dx, VGA_MISC_W
    mov al, 0x67
    out dx, al

    mov si, vga_seq             ; sequencer 1-4
    mov dx, VGA_SEQ_I
    mov cx, vga_seq_end - vga_seq
    mov ah, 1
    call vga_write_indexed
    mov ax, 0x0300              ; out of reset
    out dx, ax

    ; CRTC: unlock registers 0-7 (CR11 bit 7) before writing them
    mov dx, VGA_CRTC_I
    mov ax, 0x0E11
    out dx, ax
    mov si, vga_crtc
    mov cx, vga_crtc_end - vga_crtc
    xor ah, ah
    call vga_write_indexed

    mov si, vga_gc
    mov dx, VGA_GC_I
    mov cx, vga_gc_end - vga_gc
    xor ah, ah
    call vga_write_indexed

    ; Attribute controller: index and data share 0x3C0
    mov dx, VGA_IS1
    in al, dx
    mov si, vga_ac
    xor bl, bl
.ac:
    mov dx, VGA_AC
    mov al, bl
    out dx, al
    lodsb
    out dx, al
    inc bl
    cmp si, vga_ac_end
    jb .ac

    call vga_dac
    call vga_load_font

    ; Display on: palette address source set
    mov dx, VGA_IS1
    in al, dx
    mov dx, VGA_AC
    mov al, 0x20
    out dx, al

    ; Blank screen, light grey on black
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov ax, 0x0720
    mov cx, VGA_COLS * VGA_ROWS
    rep stosw

    pop es
    ret

; Write CX bytes from DS:SI to the indexed registers at DX (index port)
; and DX + 1 (data port), starting at index AH.
vga_write_indexed:
.next:
    lodsb                       ; AL = value, AH = index
    xchg al, ah
    out dx, ax                  ; index to DX, value to DX + 1
    xchg al, ah
    inc ah
    loop .next
    ret

; DAC: the 64 EGA colours (rgbRGB), so the attribute palette below
; gives the usual 16 text colours. 6 bits per component.
vga_dac:
    mov dx, VGA_DAC_W
    xor al, al
    out dx, al
    mov dx, VGA_DAC_DATA
    xor bl, bl
.colour:
    mov cx, 0x0205              ; red: bit 2 is 2/3, bit 5 is 1/3
    call .component
    mov cx, 0x0104              ; green: bits 1 and 4
    call .component
    mov cx, 0x0003              ; blue: bits 0 and 3
    call .component
    inc bl
    cmp bl, 64
    jb .colour
    ret
; In: BL = colour, CH = bit for 2/3 intensity, CL = bit for 1/3
.component:
    xor al, al
    mov ah, bl
    xchg cl, ch
    shr ah, cl                  ; bit CH
    xchg cl, ch
    test ah, 1
    jz .no_hi
    add al, 0x2A
.no_hi:
    mov ah, bl
    shr ah, cl                  ; bit CL
    test ah, 1
    jz .no_lo
    add al, 0x15
.no_lo:
    out dx, al
    ret

; Copy font_8x16 into plane 2, 32 bytes per character slot.
vga_load_font:
    ; Sequencer: write plane 2 only, sequential addressing
    mov dx, VGA_SEQ_I
    mov ax, 0x0402
    out dx, ax
    mov ax, 0x0704
    out dx, ax
    ; Graphics controller: read plane 2, plain writes, 0xA0000 64 KB window
    mov dx, VGA_GC_I
    mov ax, 0x0204
    out dx, ax
    mov ax, 0x0005
    out dx, ax
    mov ax, 0x0406
    out dx, ax

    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov si, font_8x16
    mov bx, 256
.char:
    mov cx, 8                   ; 16 bytes of glyph
    rep movsw
    xor ax, ax
    mov cx, 8                   ; 16 unused rows of the 32-byte slot
    rep stosw
    dec bx
    jnz .char

    ; Back to text mode: planes 0 and 1, odd/even
    mov dx, VGA_SEQ_I
    mov ax, 0x0302
    out dx, ax
    mov ax, 0x0204
    out dx, ax
    mov dx, VGA_GC_I
    mov ax, 0x0004
    out dx, ax
    mov ax, 0x1005
    out dx, ax
    mov ax, 0x0E06
    out dx, ax
    ret

; Register tables, from index 0
vga_seq:                        ; from index 1
    db 0x00                     ; clocking: 9-dot characters
    db 0x03                     ; map mask: planes 0 and 1
    db 0x00                     ; character map: font at plane 2 offset 0
    db 0x02                     ; memory mode: odd/even, extended memory
vga_seq_end:

vga_crtc:
    db 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F  ; horizontal, CR07 overflow
    db 0x00, 0x4F                                      ; preset row scan; 16 scan lines per row
    db 0x0D, 0x0E                                      ; cursor: lines 13-14
    db 0x00, 0x00, 0x00, 0x00                          ; start address, cursor at 0
    db 0x9C, 0x8E, 0x8F                                ; vertical retrace; display end 0x18F
    db 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF              ; offset 80, underline, blanking, mode
vga_crtc_end:

vga_gc:
    db 0x00, 0x00, 0x00, 0x00, 0x00
    db 0x10                     ; mode: odd/even
    db 0x0E                     ; misc: text, chain odd/even, 0xB8000 32 KB
    db 0x00, 0xFF
vga_gc_end:

vga_ac:
    db 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07  ; palette -> DAC
    db 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    db 0x0C                     ; mode: text, line graphics, blink
    db 0x00                     ; overscan
    db 0x0F                     ; colour planes
    db 0x08                     ; horizontal pan: 9-dot characters
    db 0x00                     ; colour select
vga_ac_end:

; Joke klingon-font branch: pIqaD glyphs instead of the latin font.inc.
; vga_load_font still looks up font_8x16.
%include "klingon-font.inc"
font_8x16 equ klingon_font
