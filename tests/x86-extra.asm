; Smoke-test image for the SQBL header (x86-extra-sectors). Original code.
;
; Sector 0 is entered at 0x0000:0x7C00 as usual. It checks DL and that
; ES:DI still points at the memory map, then far-jumps to the extra
; sectors, which the firmware loaded to LOAD_SEG:LOAD_OFF. The first extra
; sector prints "payload=ok" on COM1 when the marker at the end of the last
; extra sector is there too, so every sector arrived where it should.
;
; Built once per case; the defines move the load to where the firmware
; must refuse it:
;   (none)          3 sectors at 0x1000:0x0010 (linear 0x10010)
;   OVERLAP_MAP     1 sector at 0x0000:0x0400, runs into 0x0500
;   OVERLAP_BOOT    8 sectors at 0x0700:0x0000, runs into 0x7C00
;   OUT_OF_RANGE    2 sectors at 0x9000:0xFE00, crosses 0xA0000

BITS 16

COM1        equ 0x3F8
MM_ADDR     equ 0x0500
EXTRA       equ 3                       ; sectors after sector 0

%ifdef OVERLAP_MAP
LOAD_SEG    equ 0x0000
LOAD_OFF    equ 0x0400
COUNT       equ 1
%elifdef OVERLAP_BOOT
LOAD_SEG    equ 0x0700
LOAD_OFF    equ 0x0000
COUNT       equ 8
%elifdef OUT_OF_RANGE
LOAD_SEG    equ 0x9000
LOAD_OFF    equ 0xFE00
COUNT       equ 2
%else
LOAD_SEG    equ 0x1000
LOAD_OFF    equ 0x0010                  ; not segment-aligned on purpose
COUNT       equ EXTRA
%endif

; ---- sector 0 -------------------------------------------------------------

section boot start=0 vstart=0x7C00

boot:
    cld
    xor ax, ax
    mov ds, ax
    mov bl, dl                          ; puts uses DX
    mov si, msg_entry
    call puts
    mov si, msg_dl_bad
    cmp bl, 0x80
    jne .dl
    mov si, msg_dl_ok
.dl:
    call puts
    mov si, msg_mm_bad
    test di, di                         ; ES:DI = 0x0000:0x0500, 'SQMM'
    jz .mm
    cmp di, MM_ADDR
    jne .mm
    mov ax, es
    test ax, ax
    jnz .mm
    cmp dword [MM_ADDR], 'SQMM'
    jne .mm
    mov si, msg_mm_ok
.mm:
    call puts
    jmp LOAD_SEG:LOAD_OFF

; DS:SI = zero-terminated string
puts:
    lodsb
    test al, al
    jz .done
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    jmp puts
.done:
    ret

msg_entry   db 'OS-ENTRY x86 extra', 0
msg_dl_ok   db ' dl=ok', 0
msg_dl_bad  db ' dl=bad', 0
msg_mm_ok   db ' memmap=ok', 0
msg_mm_bad  db ' memmap=bad', 0

times 0x1F0 - ($ - $$) db 0
    db 'SQBL'                           ; magic
    dw COUNT                            ; extra sectors
    dw LOAD_SEG
    dw LOAD_OFF
    dd 0                                ; reserved
    dw 0xAA55

; ---- extra sectors, run at LOAD_SEG:LOAD_OFF -------------------------------

section payload follows=boot vstart=LOAD_OFF

payload:
    mov ax, cs
    mov ds, ax
    mov si, msg_bad
    cmp dword [marker], 'LAST'
    jne .out
    mov si, msg_ok
.out:
    mov dx, COM1
.putc:
    lodsb
    test al, al
    jz .halt
    out dx, al
    jmp .putc
.halt:
    cli
    hlt
    jmp .halt

msg_ok      db ' payload=ok', 13, 10, 0
msg_bad     db ' payload=bad', 13, 10, 0

times EXTRA * 512 - 4 - ($ - $$) db 0
marker      db 'LAST'
