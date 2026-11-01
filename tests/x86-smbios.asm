; Smoke-test: find the SMBIOS entry point and check its checksums.
; Original code.
;
; Scans 0xF0000–0xFFFFF every 16 bytes for "_SM_", verifies the EPS
; checksum and the intermediate "_DMI_" checksum, and checks that the
; structure table begins with Type 0. Prints smbios=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8

start:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00

    mov si, msg_entry
    call puts

    mov edx, 0xF0000
.scan:
    mov eax, edx
    call lin_es_bx
    cmp dword [es:bx], '_SM_'
    jne .next

    movzx cx, byte [es:bx + 5]  ; length
    cmp cx, 0x1F
    jb .next
    cmp cx, 32
    ja .next
    xor al, al
    push si
    mov si, bx
.eps:
    add al, [es:si]
    inc si
    loop .eps
    pop si
    test al, al
    jnz .next

    ; Intermediate: offsets 0x10 .. length-1
    movzx cx, byte [es:bx + 5]
    sub cx, 0x10
    xor al, al
    push si
    lea si, [bx + 0x10]
.inter:
    add al, [es:si]
    inc si
    loop .inter
    pop si
    test al, al
    jnz .next

    cmp dword [es:bx + 0x10], '_DMI'
    jne .bad
    cmp byte [es:bx + 0x14], '_'
    jne .bad

    mov eax, [es:bx + 0x18]     ; table address
    call lin_es_bx
    cmp byte [es:bx], 0         ; Type 0 BIOS Information
    jne .bad

    mov si, msg_ok
    jmp .done

.next:
    add edx, 16
    cmp edx, 0x100000
    jb .scan
.bad:
    mov si, msg_bad
.done:
    call puts
    mov si, msg_crlf
    call puts
.halt:
    cli
    hlt
    jmp .halt

lin_es_bx:
    mov ebx, eax
    shr eax, 4
    mov es, ax
    and bx, 0x000F
    ret

puts:
    lodsb
    test al, al
    jz .r
    call putc
    jmp puts
.r: ret

putc:
    mov ah, al
    mov dx, COM1 + 5
.w: in al, dx
    test al, 0x20
    jz .w
    mov al, ah
    mov dx, COM1
    out dx, al
    ret

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'smbios=ok', 0
msg_bad   db 'smbios=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
