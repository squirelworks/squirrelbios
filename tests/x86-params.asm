; Smoke-test: INT 13h AH=08h only reports drives that answered IDENTIFY.
; Original code.
;
; Booted from the primary master. AH=08h with DL = 0x80 must succeed and
; report SLAVE+1 hard disks in DL. With DL = 0x81 it must succeed when the
; slave is present (-DSLAVE) and otherwise return CF = 1, AH = 07h.
; Prints params=ok or params=bad:<step>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
%ifdef SLAVE
DISKS   equ 2
%else
DISKS   equ 1
%endif

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

    ; Master: present, DL = number of disks
    mov byte [step], 'm'
    mov ah, 0x08
    mov dl, 0x80
    int 0x13
    jc bad
    test ah, ah
    jnz bad
    cmp dl, DISKS
    jne bad

    ; Slave
    mov byte [step], 's'
    mov ah, 0x08
    mov dl, 0x81
    int 0x13
%ifdef SLAVE
    jc bad
    test ah, ah
    jnz bad
    cmp dl, DISKS
    jne bad
%else
    jnc bad
    cmp ah, 0x07
    jne bad
%endif

    ; A drive number that is not a hard disk here is still invalid
    mov byte [step], 'x'
    mov ah, 0x08
    mov dl, 0x82
    int 0x13
    jnc bad

    ; BDA 0x475 agrees
    mov byte [step], 'b'
    cmp byte [0x475], DISKS
    jne bad

    mov si, msg_ok
    call puts
    jmp halt

bad:
    mov si, msg_bad
    call puts
    mov al, [step]
    call putc
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
    push dx
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    pop dx
    ret

step      db 0
msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'params=ok', 13, 10, 0
msg_bad   db 'params=bad:', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
