; Smoke-test: PCI enumeration and the PCI BIOS, on i440fx and q35.
; Original code.
;
; Sector 0 carries an SQBL header that loads two more sectors right after
; it, at 0x7E00, so the image runs as one piece from 0x7C00. QEMU is
; started with -device virtio-rng-pci (transitional: an I/O BAR and a
; 64-bit memory BAR). Steps:
;   i  INT 1Ah AX=B101: CF clear, AH = 0, EDX = 'PCI ', AL bit 0, BX = 0210h
;   h  AX=B103 class 060000h index 0 is 00:00.0, and AX=B10A reads its ID:
;      8086:1237 (i440fx) or 8086:29C0 (q35)
;   v  AX=B102 finds the std VGA (1234:1111); BAR0 is a prefetchable
;      memory BAR at or above E0000000h, 16 MB aligned; memory decoding on
;   o  AX=B102 finds virtio-rng (1AF4:1005); BAR0 is an I/O BAR at or
;      above C000h, BAR4 a 64-bit memory BAR with BAR5 = 0; I/O and
;      memory decoding on
;   n  AX=B102 for 1234:BEEF: CF set, AH = 86h
;   r  AX=B109 at an odd register: CF set, AH = 87h; AX=B10B / B108
;      write and read back virtio-rng's Interrupt Line
;   q  i440fx: virtio-rng's Interrupt Line (written before r) is the IRQ
;      the PIIX3 routes its INTA to; q35: it is FFh
;   p  i440fx: a '$PIR' table on a 16-byte boundary in F0000h-FFFFFh,
;      byte sum 0, router 00:01.0
; Prints pci=ok, or pci=bad:<step> <AH in hex>.

BITS 16
ORG 0x7C00

COM1    equ 0x3F8
EXTRA   equ 2

start:
    jmp 0:main

times 0x1F0 - ($ - $$) db 0
    db 'SQBL'
    dw EXTRA
    dw 0                            ; segment
    dw 0x7E00                       ; offset
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

    mov byte [step], 'i'
    xor edx, edx
    mov ax, 0xB101
    int 0x1A
    jc bad
    test ah, ah
    jnz bad
    cmp edx, 0x20494350
    jne bad
    test al, 1
    jz bad
    cmp bx, 0x0210
    jne bad

    mov byte [step], 'h'
    mov ecx, 0x060000
    xor si, si
    mov ax, 0xB103
    int 0x1A
    jc bad
    test bx, bx
    jnz bad
    xor di, di
    mov ax, 0xB10A
    int 0x1A
    jc bad
    cmp ecx, 0x12378086
    je .i440fx
    cmp ecx, 0x29C08086
    jne bad
    mov byte [q35], 1
.i440fx:

    mov byte [step], 'v'
    mov cx, 0x1111
    mov dx, 0x1234
    call find
    mov di, 0x10
    call rd32
    mov eax, ecx
    and al, 0x0F
    cmp al, 0x08                    ; memory, 32-bit, prefetchable
    jne bad
    cmp ecx, 0xE0000000
    jb bad
    test ecx, 0x00FFFFF0
    jnz bad
    call cmd
    test cl, 2
    jz bad

    mov byte [step], 'o'
    mov cx, 0x1005
    mov dx, 0x1AF4
    call find
    mov [rng], bx
    mov di, 0x10
    call rd32
    test cl, 1
    jz bad
    cmp ecx, 0xC000
    jb bad
    mov di, 0x20
    call rd32
    mov al, cl
    and al, 0x07
    cmp al, 0x04                    ; memory, 64-bit
    jne bad
    cmp ecx, 0xE0000000
    jb bad
    mov di, 0x24
    call rd32
    test ecx, ecx
    jnz bad
    call cmd
    and cl, 3
    cmp cl, 3
    jne bad

    mov byte [step], 'n'
    mov cx, 0xBEEF
    mov dx, 0x1234
    xor si, si
    mov ax, 0xB102
    int 0x1A
    jnc bad
    cmp ah, 0x86
    jne bad

    ; Interrupt Line as POST left it, for step q
    mov bx, [rng]
    mov di, 0x3C
    mov ax, 0xB108
    int 0x1A
    jc bad
    mov [line], cl

    mov byte [step], 'r'
    mov di, 0x3D
    mov ax, 0xB109
    int 0x1A
    jnc bad
    cmp ah, 0x87
    jne bad
    mov di, 0x3C
    mov cl, 0x5A
    mov ax, 0xB10B
    int 0x1A
    jc bad
    xor cl, cl
    mov ax, 0xB108
    int 0x1A
    jc bad
    cmp cl, 0x5A
    jne bad

    mov byte [step], 'q'
    mov al, [line]
    cmp byte [q35], 0
    je .route
    cmp al, 0xFF
    jne bad
    jmp ok
.route:
    ; PIRQ (device - 1 + INTA) mod 4: route register 0x60 + that, on
    ; the PIIX3 at 00:01.0
    mov al, [rng]
    shr al, 3
    dec al
    and al, 3
    movzx di, al
    add di, 0x60
    mov bx, 0x0008
    mov ax, 0xB108
    int 0x1A
    jc bad
    cmp cl, [line]
    jne bad

    mov byte [step], 'p'
    mov ax, 0xF000
    mov es, ax
    xor di, di
.scan:
    cmp dword [es:di], '$PIR'
    je .pir
    add di, 16
    jnz .scan
    jmp bad
.pir:
    mov cx, [es:di + 6]
    xor al, al
    mov si, di
.sum:
    add al, [es:si]
    inc si
    loop .sum
    test al, al
    jnz bad
    cmp word [es:di + 8], 0x0800    ; bus 0, devfn 08h
    jne bad

ok:
    mov si, msg_ok
    call puts
    jmp halt

; CX = device, DX = vendor: BX = its bus / devfn, or bad
find:
    xor si, si
    mov ax, 0xB102
    int 0x1A
    jc bad_pop
    test ah, ah
    jnz bad_pop
    ret

; DI = register of BX: ECX = the dword, or bad
rd32:
    mov ax, 0xB10A
    int 0x1A
    jc bad_pop
    ret

; CX = BX's command register, or bad
cmd:
    mov di, 0x04
    mov ax, 0xB109
    int 0x1A
    jc bad_pop
    ret

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

step      db 0
status    db 0
q35       db 0
line      db 0
rng       dw 0
msg_entry db 'OS-ENTRY x86 pci ', 0
msg_ok    db 'pci=ok', 13, 10, 0
msg_bad   db 'pci=bad:', 0
msg_crlf  db 13, 10, 0

times (EXTRA + 1) * 512 - ($ - $$) db 0
