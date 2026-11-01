; Smoke image shaped like a Squirrel-OS/2 hard disk. Original code.
;
; Sector 0 has no SQBL header. If the firmware enters it, that is the
; one-sector hand-off and this sector says so. The 192 sectors after it
; are the "kernel": the last dword is 'S2OK', and the first bytes run at
; 0x8000. They check the hand-off registers, that INT 15h E820 returns
; the SQMM table, and that INT 13h is not a disk service.

BITS 16

section boot vstart=0x7C00

COM1        equ 0x3F8
OS_SECTORS  equ 192
OS_SIG      equ 0x4B4F3253          ; 'S2OK'
SIG_LIN     equ 0x8000 + OS_SECTORS * 512 - 4
FLAG_IF     equ 0x0200
FLAG_DF     equ 0x0400
VARS        equ 0x7000
saved_flags equ VARS
saved_dx    equ VARS + 2
saved_di    equ VARS + 4

boot:
    xor ax, ax
    mov ds, ax
    mov si, msg_entered
    call bputs
.halt:
    cli
    hlt
    jmp .halt

bputs:
    lodsb
    test al, al
    jz .done
    call bputc
    jmp bputs
.done:
    ret

bputc:
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

msg_entered db 'entered-boot-sector', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55

; Loaded at 0x8000. vstart is that address; the bytes follow sector 0 in the file.
section kernel vstart=0x8000

stage2:
    pushf
    pop word [saved_flags]
    mov [saved_dx], dx
    mov [saved_di], di
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_load
    call puts

    mov si, msg_dl_ok
    cmp byte [saved_dx], 0x80
    je .dl
    mov si, msg_dl_bad
.dl:
    call puts

    mov si, msg_fl_ok
    test word [saved_flags], FLAG_IF | FLAG_DF
    jz .fl
    mov si, msg_fl_bad
.fl:
    call puts

    mov si, msg_mm_ok
    cmp word [saved_di], 0x0500
    jne .mmbad
    cmp dword [0x0500], 'SQMM'
    je .mm
.mmbad:
    mov si, msg_mm_bad
.mm:
    call puts

    mov si, msg_sig_ok
    push es
    mov ax, SIG_LIN >> 4
    mov es, ax
    cmp dword [es:SIG_LIN & 0x0F], OS_SIG
    pop es
    je .sig
    mov si, msg_sig_bad
.sig:
    call puts

    call check_e820
    mov si, msg_e820_ok
    jnc .e8
    mov si, msg_e820_bad
.e8:
    call puts

    mov ah, 0x42
    xor bx, bx
    int 0x13
    mov si, msg_disk_abs
    jc .dk
    mov si, msg_disk_pres
.dk:
    call puts

    mov si, msg_crlf
    call puts

halt:
    cli
    hlt
    jmp halt

; Walk INT 15h E820 and compare each 20-byte entry with the SQMM table.
; CF = 1 if the call fails, the bytes differ, or the continuation is wrong.
check_e820:
    xor ebx, ebx
    mov di, 0x1000
    mov si, 0x0508
    mov cx, [0x0504]
    test cx, cx
    jz .bad
    cmp cx, 32
    ja .bad
.entry:
    push cx
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [di + 20], 1
    int 0x15
    jc .popbad
    cmp eax, 0x534D4150
    jne .popbad
    cmp ecx, 20
    jne .popbad
    push si
    push di
    mov cx, 10
    repe cmpsw
    pop di
    pop si
    jne .popbad
    add si, 20
    add di, 24
    pop cx
    dec cx
    jz .last
    test ebx, ebx
    jz .bad
    jmp .entry
.last:
    test ebx, ebx
    jnz .bad
    clc
    ret
.popbad:
    pop cx
.bad:
    stc
    ret

puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
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

msg_load     db 'osload=ok', 0
msg_dl_ok    db ' dl=ok', 0
msg_dl_bad   db ' dl=bad', 0
msg_fl_ok    db ' flags=ok', 0
msg_fl_bad   db ' flags=bad', 0
msg_mm_ok    db ' memmap=ok', 0
msg_mm_bad   db ' memmap=bad', 0
msg_sig_ok   db ' sig=ok', 0
msg_sig_bad  db ' sig=bad', 0
msg_e820_ok  db ' e820=ok', 0
msg_e820_bad db ' e820=bad', 0
msg_disk_abs db ' disk=absent', 0
msg_disk_pres db ' disk=present', 0
msg_crlf     db 13, 10, 0

times OS_SECTORS * 512 - 4 - ($ - $$) db 0
image_sig:
    dd OS_SIG
