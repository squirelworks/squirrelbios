; Smoke-test: find the ACPI RSDP in the BIOS ROM area and check it.
; Original code.
;
; Scans 0xE0000–0xFFFFF every 16 bytes for "RSD PTR ", verifies the
; ACPI 1.0 checksum over the first 20 bytes, checks that the RSDT
; signature and checksum are valid and that entry 0 is FACP. Prints
; acpi=ok.

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

    ; EDX = linear address 0xE0000 .. 0xFFFF0 step 16
    mov edx, 0xE0000
.scan:
    mov eax, edx
    shr eax, 4
    mov es, ax
    mov bx, dx
    and bx, 0x000F
    cmp dword [es:bx], 'RSD '
    jne .next
    cmp dword [es:bx + 4], 'PTR '
    jne .next

    mov cx, 20
    xor al, al
    push si
    mov si, bx
.sum:
    add al, [es:si]
    inc si
    loop .sum
    pop si
    test al, al
    jnz .next

    ; RSDT physical address at offset 16
    mov eax, [es:bx + 16]
    call lin_es_bx
    cmp dword [es:bx], 'RSDT'
    jne .bad
    mov cx, [es:bx + 4]
    cmp cx, 40
    jb .bad
    cmp cx, 256
    ja .bad
    xor al, al
    push si
    mov si, bx
.rsum:
    add al, [es:si]
    inc si
    loop .rsum
    pop si
    test al, al
    jnz .bad

    mov eax, [es:bx + 36]       ; first SDT pointer
    call lin_es_bx
    cmp dword [es:bx], 'FACP'
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

; EAX = linear address. Out: ES:BX = that address (BX < 16).
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
msg_ok    db 'acpi=ok', 0
msg_bad   db 'acpi=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
