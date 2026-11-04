; Smoke-test: EDD 3.0 and LBA48 on a 200 GiB disk.
; Original code.
;
; tests/mkbigdisk.py makes a sparse 200 GiB raw disk with this image at
; LBA 0 and 'LBA:' + LBA tags at the sectors the steps read. The SQBL
; header loads the two sectors after the boot sector to 0x7E00. Steps:
;   e  AH=41h BX=55AAh: CF clear, BX = AA55h, AH = 30h, CX bit 0
;   p  AH=48h with a 42h-byte buffer: size 42h, 419430400 sectors,
;      512 bytes per sector, key BEDDh, 'ATA', byte sum 1Eh-41h is 0
;   s  AH=48h with a 1Ah-byte buffer: size 1Ah, the same sector count
;   l  AH=42h, 2 sectors from 0FFFFFFFh: the range crosses 2^28, so
;      LBA48 commands; both tags arrive
;   h  AH=42h at LBA 300000000 (143 GiB in): its tag
;   w  AH=43h writes LBA 10000010h, AH=42h reads it back
;   v  AH=44h on the last sector: CF clear; one past it: CF set, AH = 04h
;   b  AH=42h with the LBA's upper dword set: CF set, AH = 01h
; Prints edd=ok, or edd=bad:<step> <AH in hex>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
EXTRA   equ 2
BUF     equ 0x2000                  ; segment of the read buffer
SECTORS equ 200 * 2 * 1024 * 1024   ; 200 GiB

start:
    jmp 0:main

times 0x1F0 - ($ - $$) db 0
    db 'SQBL'
    dw EXTRA
    dw 0
    dw 0x7E00
    dd 0
    dw 0xAA55

main:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    mov si, msg_entry
    call puts

    mov byte [step], 'e'
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, 0x80
    int 0x13
    jc bad
    cmp ah, 0x30
    jne bad
    cmp bx, 0xAA55
    jne bad
    test cl, 1
    jz bad

    mov byte [step], 'p'
    mov word [params], 0x42
    mov si, params
    mov ah, 0x48
    mov dl, 0x80
    int 0x13
    jc bad
    cmp word [params], 0x42
    jne bad
    cmp dword [params + 16], SECTORS
    jne bad
    cmp dword [params + 20], 0
    jne bad
    cmp word [params + 24], 512
    jne bad
    cmp word [params + 0x1E], 0xBEDD
    jne bad
    cmp dword [params + 0x28], 'ATA '
    jne bad
    mov si, params + 0x1E
    mov cx, 0x24
    xor al, al
.sum:
    add al, [si]
    inc si
    loop .sum
    test al, al
    jnz bad

    mov byte [step], 's'
    mov dword [params + 16], 0
    mov word [params], 0x1A
    mov si, params
    mov ah, 0x48
    mov dl, 0x80
    int 0x13
    jc bad
    cmp word [params], 0x1A
    jne bad
    cmp dword [params + 16], SECTORS
    jne bad

    mov byte [step], 'l'
    mov eax, 0x0FFFFFFF
    mov cx, 2
    call read
    mov eax, 0x0FFFFFFF
    xor di, di
    call tag
    mov eax, 0x10000000
    mov di, 512
    call tag

    mov byte [step], 'h'
    mov eax, 300000000
    mov cx, 1
    call read
    mov eax, 300000000
    xor di, di
    call tag

    mov byte [step], 'w'
    mov ax, BUF
    mov es, ax
    mov dword [es:0], 'LBA:'
    mov dword [es:4], 0x10000010
    mov dword [dap_lba], 0x10000010
    mov word [dap_count], 1
    mov si, dap
    mov ah, 0x43
    mov dl, 0x80
    int 0x13
    jc bad
    mov dword [es:0], 0
    mov eax, 0x10000010
    mov cx, 1
    call read
    mov eax, 0x10000010
    xor di, di
    call tag

    mov byte [step], 'v'
    mov dword [dap_lba], SECTORS - 1
    mov word [dap_count], 1
    mov si, dap
    mov ah, 0x44
    mov dl, 0x80
    int 0x13
    jc bad
    mov dword [dap_lba], SECTORS
    mov ah, 0x44
    int 0x13
    jnc bad
    cmp ah, 0x04
    jne bad

    mov byte [step], 'b'
    mov dword [dap_lba], 0
    mov dword [dap_lba + 4], 1
    mov ah, 0x42
    int 0x13
    jnc bad
    cmp ah, 0x01
    jne bad

    mov si, msg_ok
    call puts
    jmp halt

; EAX = LBA, CX = count: AH=42h into BUF:0, or bad
read:
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0
    mov [dap_count], cx
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc bad_pop
    ret

; EAX = LBA the sector at BUF:DI should carry, or bad
tag:
    push es
    mov bx, BUF
    mov es, bx
    cmp dword [es:di], 'LBA:'
    jne .no
    cmp [es:di + 4], eax
    jne .no
    pop es
    ret
.no:
    pop es
    xor ah, ah
    jmp bad_pop

bad_pop:
    add sp, 2
bad:
    mov [status], ah
    mov si, msg_bad
    call puts
    mov al, [step]
    call putc
    mov al, ' '
    call putc
    mov al, [status]
    shr al, 4
    call hexdigit
    mov al, [status]
    and al, 0x0F
    call hexdigit
    mov si, msg_crlf
    call puts
halt:
    cli
    hlt
    jmp halt

hexdigit:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 7
    jmp putc

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

align 4
dap:
    db 0x10, 0
dap_count:
    dw 1
    dw 0, BUF                       ; offset, segment
dap_lba:
    dq 0

step      db 0
status    db 0
msg_entry db 'OS-ENTRY x86 edd ', 0
msg_ok    db 'edd=ok', 13, 10, 0
msg_bad   db 'edd=bad:', 0
msg_crlf  db 13, 10, 0

params    times 0x42 db 0

times (EXTRA + 1) * 512 - ($ - $$) db 0
