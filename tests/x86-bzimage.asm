; Minimal Linux-style bzImage for the firmware's HdrS path.
; Original code.
;
; File layout:
;   0x000–0x1FF  boot sector: setup_sects, syssize, AA55
;   0x200–0x3FF  setup: HdrS, version, loadflags, code32_start
;   0x400–…      32-bit payload (vstart 0x100000) that prints bz=ok

BITS 16

section hdr start=0
times 0x1F1 db 0
db 1                            ; setup_sects
dw 0                            ; root_flags
dd (pm_end - pm_start + 15) / 16
times 0x1FE - ($ - $$) db 0
dw 0xAA55

times 0x202 - ($ - $$) db 0
db 'HdrS'
dw 0x0204
times 0x211 - ($ - $$) db 0
db 0x01                         ; LOADED_HIGH
times 0x214 - ($ - $$) db 0
dd 0x00100000                   ; code32_start
times 0x400 - ($ - $$) db 0

BITS 32
section pm start=0x400 vstart=0x100000
pm_start:
    mov esi, msg
.putc:
    lodsb
    test al, al
    jz .halt
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    jmp .putc
.halt:
    hlt
    jmp .halt

msg db 'bz=ok', 13, 10, 0
pm_end:
