; Smoke-test: INT 1Ah ticks and RTC time.
; Original code.
;
; STI until AH=0x00 reports a non-zero tick count from IRQ0, then AH=0x02
; returns BCD hours/minutes/seconds in range. Prints time=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
FLAG_IF         equ 0x0200
MAX_HALTS       equ 64

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov si, msg_entry
    call puts

    ; Unknown AH -> CF set
    mov ah, 0x99
    int 0x1A
    jnc .bad

    mov cx, MAX_HALTS
    sti
.wait:
    xor ah, ah
    int 0x1A                    ; CX:DX = ticks, AL = overflow
    mov ax, cx
    or ax, dx
    jnz .ticked
    hlt
    loop .wait
    cli
    jmp .bad

.ticked:
    mov ah, 0x02
    int 0x1A
    jc .bad_cli
    ; CH=hours BCD, CL=minutes, DH=seconds
    mov al, ch
    call check_bcd_hour
    jc .bad_cli
    mov al, cl
    call check_bcd_60
    jc .bad_cli
    mov al, dh
    call check_bcd_60
    jc .bad_cli

    cli
    pushf
    pop ax
    test ax, FLAG_IF
    jnz .bad

    mov si, msg_ok
    jmp .done
.bad_cli:
    cli
.bad:
    mov si, msg_bad
.done:
    call puts
    mov si, msg_crlf
    call puts

halt:
    cli
    hlt
    jmp halt

; AL = BCD hour: high nibble 0-2, value <= 0x23. CF set if bad.
check_bcd_hour:
    call check_bcd_nibbles
    jc .no
    cmp al, 0x24
    jae .no
    clc
    ret
.no:
    stc
    ret

; AL = BCD minutes/seconds: <= 0x59. CF set if bad.
check_bcd_60:
    call check_bcd_nibbles
    jc .no
    cmp al, 0x60
    jae .no
    clc
    ret
.no:
    stc
    ret

; Each nibble must be 0-9.
check_bcd_nibbles:
    push ax
    mov ah, al
    and al, 0x0F
    cmp al, 9
    ja .bad
    mov al, ah
    shr al, 4
    cmp al, 9
    ja .bad
    pop ax
    clc
    ret
.bad:
    pop ax
    stc
    ret

puts:
    lodsb
    test al, al
    jz .ret
    call putc
    jmp puts
.ret:
    ret

putc:
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    ret

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'time=ok', 0
msg_bad   db 'time=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
