; Smoke-test: PIT ~18.2 Hz and PIC IRQ0 at INT 08h.
; Original code.
;
; Hand-off leaves IF clear. STI, HLT until BDA_TICKS (0x046C) advances,
; CLI, and confirm IF is clear again. Prints pit=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
BDA_TICKS       equ 0x046C
FLAG_IF         equ 0x0200
; At ~18.2 Hz a tick is ~55 ms. Bound HLT wakeups so a stuck IRQ fails.
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

    pushf
    pop ax
    test ax, FLAG_IF
    jnz .bad

    mov ebx, [BDA_TICKS]
    mov cx, MAX_HALTS
    sti
.wait:
    hlt
    mov eax, [BDA_TICKS]
    cmp eax, ebx
    jne .ticked
    loop .wait
    cli
    jmp .bad

.ticked:
    cli
    pushf
    pop ax
    test ax, FLAG_IF
    jnz .bad

    mov si, msg_ok
    jmp .done
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
msg_ok    db 'pit=ok', 0
msg_bad   db 'pit=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
