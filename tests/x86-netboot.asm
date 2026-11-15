; Smoke-test: network boot. Original code.
;
; Served by QEMU's TFTP server and loaded at 0x7C00 as a whole file of
; SIZE bytes (not a multiple of 512, so the last TFTP block is short;
; build with -DSIZE=<n> for others). Checks that it was entered with
; DL = FFh, and that every byte after the first sector arrived: byte i
; of the file holds i * 7 + (i >> 8), and the last dword is 'NETE'.
; Prints netboot=ok, or netboot=bad.

BITS 16
ORG 0x7C00

%ifndef SIZE
SIZE    equ 5000
%endif
COM1    equ 0x3F8

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov bl, dl
    mov si, msg_entry
    call puts
    cmp bl, 0xFF
    jne .bad
    push word (0x7C00 + SIZE - 4) >> 4
    pop es
    cmp dword [es:(0x7C00 + SIZE - 4) & 0x0F], 'NETE'
    jne .bad
    ; The pattern, from offset 512 to SIZE - 4
    mov ebx, 512
.check:
    cmp ebx, SIZE - 4
    jae .ok
    mov eax, ebx
    imul eax, eax, 7
    mov ecx, ebx
    shr ecx, 8
    add eax, ecx
    mov edi, ebx
    add edi, 0x7C00
    mov ecx, edi
    shr ecx, 4
    mov es, cx
    and di, 0x0F
    cmp [es:di], al
    jne .bad
    inc ebx
    jmp .check
.ok:
    mov si, msg_ok
    jmp .done
.bad:
    mov si, msg_bad
.done:
    call puts
.halt:
    hlt
    jmp .halt

puts:
    lodsb
    test al, al
    jz .ret
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
.ret:
    ret

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'netboot=ok', 13, 10, 0
msg_bad   db 'netboot=bad', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55

%assign i 512
%rep SIZE - 4 - 512
    db ((i * 7 + (i >> 8)) & 0xFF)
%assign i i + 1
%endrep
    db 'NETE'
