; Smoke-test: INT 15h A20 control (AX=2400/2401/2402).
; Original code.
;
; Queries A20 (on after hand-off), disables, queries off, enables, queries
; on again. Confirms E820 still works. Prints int15a20=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
MM_ADDR         equ 0x0500

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

    ; Query: should be on
    mov ax, 0x2402
    int 0x15
    jc .bad
    cmp al, 1
    jne .bad

    ; Disable
    mov ax, 0x2400
    int 0x15
    jc .bad
    mov ax, 0x2402
    int 0x15
    jc .bad
    test al, al
    jnz .bad

    ; Enable
    mov ax, 0x2401
    int 0x15
    jc .bad
    mov ax, 0x2402
    int 0x15
    jc .bad
    cmp al, 1
    jne .bad

    ; Support: port 0x92 bit
    mov ax, 0x2403
    int 0x15
    jc .bad
    test bx, 0x0002
    jz .bad

    ; E820 still returns SMAP
    xor ebx, ebx
    mov edx, 0x534D4150
    mov eax, 0xE820
    mov ecx, 24
    mov di, 0x1000
    int 0x15
    jc .bad
    cmp eax, 0x534D4150
    jne .bad

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
msg_ok    db 'int15a20=ok', 0
msg_bad   db 'int15a20=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
