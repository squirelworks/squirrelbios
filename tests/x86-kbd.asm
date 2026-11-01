; Smoke-test: INT 16h keyboard with IRQ1 feeding the BDA buffer.
; Original code.
;
; Expects the harness to inject scancode for 'a' (QEMU sendkey a) after
; the firmware banner. Checks AH=01 then AH=00, refuses an unknown AH
; with CF set, leaves IF clear, prints kbd=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
BDA_TICKS       equ 0x046C
FLAG_IF         equ 0x0200
; Wait up to ~2 s of PIT ticks (~36 at 18.2 Hz) for the injected key.
MAX_TICKS       equ 40

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

    ; Unknown INT 16h function -> CF set
    mov ah, 0x99
    int 0x16
    jnc .bad

    mov ebx, [BDA_TICKS]
    sti
.wait:
    mov ah, 0x01
    int 0x16
    jnz .ready                  ; ZF clear => key pending
    mov eax, [BDA_TICKS]
    sub eax, ebx
    cmp eax, MAX_TICKS
    jae .bad_cli
    hlt
    jmp .wait

.ready:
    mov ah, 0x00
    int 0x16
    cli
    cmp al, 'a'
    jne .bad
    cmp ah, 0x1E                ; set-1 make code for A
    jne .bad
    pushf
    pop bx
    test bx, FLAG_IF
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
msg_ok    db 'kbd=ok', 0
msg_bad   db 'kbd=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
