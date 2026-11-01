; Smoke-test: A20 is enabled before hand-off.
; Original code.
;
; Unreal mode (4 GB data-segment limit) writes a pattern at 0x500000,
; then proves A20 is open: 0x100500 and 0x000500 are not aliases.
; Prints a20=ok.

BITS 16
ORG 0x7C00

COM1            equ 0x3F8
TEST_HI         equ 0x500000    ; high RAM the TODO asks us to touch
ALIAS_HI        equ 0x100500    ; same as 0x000500 iff A20 is off
ALIAS_LO        equ 0x000500
PATTERN_HI      equ 0xA55A5AA5
PATTERN_A       equ 0x11111111
PATTERN_B       equ 0x22222222

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

    call enter_unreal

    ; --- write at 0x500000 and confirm it sticks ---
    mov eax, [dword TEST_HI]
    mov [saved_hi], eax
    mov dword [dword TEST_HI], PATTERN_HI
    mov eax, [dword TEST_HI]
    cmp eax, PATTERN_HI
    jne .bad_restore_hi

    ; --- A20: 0x100500 must not alias 0x000500 ---
    mov eax, [dword ALIAS_HI]
    mov [saved_alias], eax
    mov eax, [ALIAS_LO]
    mov [saved_lo], eax

    mov dword [dword ALIAS_HI], PATTERN_A
    mov dword [ALIAS_LO], PATTERN_B

    mov eax, [dword ALIAS_HI]
    cmp eax, PATTERN_A
    jne .bad_restore_all
    mov eax, [ALIAS_LO]
    cmp eax, PATTERN_B
    jne .bad_restore_all

    ; restore
    mov eax, [saved_alias]
    mov [dword ALIAS_HI], eax
    mov eax, [saved_lo]
    mov [ALIAS_LO], eax
    mov eax, [saved_hi]
    mov [dword TEST_HI], eax

    mov si, msg_ok
    jmp .done

.bad_restore_all:
    mov eax, [saved_alias]
    mov [dword ALIAS_HI], eax
    mov eax, [saved_lo]
    mov [ALIAS_LO], eax
.bad_restore_hi:
    mov eax, [saved_hi]
    mov [dword TEST_HI], eax
    mov si, msg_bad
.done:
    call puts
    mov si, msg_crlf
    call puts

halt:
    cli
    hlt
    jmp halt

; Leave DS/ES with a 4 GB limit (unreal mode). Do not reload them after
; clearing PE, or the limit falls back to 64 KB.
enter_unreal:
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp .pm
.pm:
    mov ax, 0x08
    mov ds, ax
    mov es, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp .rm
.rm:
    ret

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

align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

saved_hi    dd 0
saved_alias dd 0
saved_lo    dd 0

msg_entry db 'OS-ENTRY x86 ', 0
msg_ok    db 'a20=ok', 0
msg_bad   db 'a20=bad', 0
msg_crlf  db 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
