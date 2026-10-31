; Smoke-test boot sector for SquirrelBIOS x86.
; Stands in for the OS: checks the hand-off state and reports on COM1,
; which the firmware has already programmed. Then lists the memory map,
; one "mm <base> <length> <type>" line per entry, for the RAM-size cases.
; Original code.

BITS 16
ORG 0x7C00

COM1        equ 0x3F8
MM_ADDR     equ 0x0500
MM_END      equ 0x0600          ; the table must not run past this
MM_ENTRY    equ 20
FLAG_IF     equ 0x0200
FLAG_DF     equ 0x0400

start:
    pushf
    pop word [saved_flags]
    cld                         ; DF is checked below, not relied on
    mov [saved_dx], dx
    mov [saved_di], di

    mov si, msg_entry
    call puts

    mov si, msg_dl_ok
    cmp byte [saved_dx], 0x80
    je .dl
    mov si, msg_dl_bad
.dl:
    call puts

    call check_memmap
    mov si, msg_mm_ok
    jnc .mm
    mov si, msg_mm_bad
.mm:
    call puts

    mov si, msg_flags_ok
    test word [saved_flags], FLAG_IF | FLAG_DF
    jz .flags
    mov si, msg_flags_bad
.flags:
    call puts

    call dump_memmap

.halt:
    cli
    hlt
    jmp .halt

; Check the memory map against common/handoff.md.  CF = 1 if it is wrong.
check_memmap:
    cmp word [saved_di], MM_ADDR
    jne .bad
    cmp dword [MM_ADDR], 'SQMM'
    jne .bad
    cmp word [MM_ADDR + 6], MM_ENTRY
    jne .bad
    mov cx, [MM_ADDR + 4]
    test cx, cx
    jz .bad
    cmp cx, (MM_END - MM_ADDR - 8) / MM_ENTRY
    ja .bad

    mov si, MM_ADDR + 8
    xor ebx, ebx                ; EDX:EBX = end of the previous entry
    xor edx, edx
    xor bp, bp                  ; one bit per required region found
.entry:
    ; Sorted and not overlapping: base >= previous end
    mov eax, [si]
    mov edi, [si + 4]
    cmp edi, edx
    jb .bad
    ja .sorted
    cmp eax, ebx
    jb .bad
.sorted:
    mov ebx, eax
    mov edx, edi
    add ebx, [si + 8]
    adc edx, [si + 12]

    ; Required regions match exactly: base, length and type
    push cx
    mov di, req_regions
    mov cx, 1
.req:
    cmp eax, [di]
    jne .req_next
    cmp dword [si + 4], 0
    jne .req_next
    push eax
    mov eax, [di + 4]
    cmp eax, [si + 8]
    pop eax
    jne .req_next
    cmp dword [si + 12], 0
    jne .req_next
    push eax
    mov eax, [di + 8]
    cmp eax, [si + 16]
    pop eax
    jne .req_next
    or bp, cx
.req_next:
    add di, 12
    shl cx, 1
    cmp di, req_regions_end
    jb .req
    pop cx

    add si, MM_ENTRY
    loop .entry

    cmp bp, (1 << ((req_regions_end - req_regions) / 12)) - 1
    jne .bad
    clc
    ret
.bad:
    stc
    ret

; Print each entry as "mm " + 16 hex digits base, length, then the type.
dump_memmap:
    cmp dword [MM_ADDR], 'SQMM'
    jne .done
    mov bp, [MM_ADDR + 4]
    cmp bp, (MM_END - MM_ADDR - 8) / MM_ENTRY
    ja .done
    mov bx, MM_ADDR + 8
.entry:
    test bp, bp
    jz .done
    mov si, msg_mm
    call puts
    call hex64                  ; base
    add bx, 8
    call hex64                  ; length
    mov al, [bx + 8]            ; type
    add al, '0'
    call putc
    mov si, msg_crlf
    call puts
    add bx, MM_ENTRY - 8
    dec bp
    jmp .entry
.done:
    ret

; Qword at [BX] as 16 hex digits and a space
hex64:
    mov eax, [bx + 4]
    call hex32
    mov eax, [bx]
    call hex32
    mov al, ' '
    jmp putc

; EAX as 8 hex digits
hex32:
    mov cx, 8
.digit:
    rol eax, 4
    push eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 'A' - '9' - 1
.out:
    call putc
    pop eax
    loop .digit
    ret

puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
    ret

; AL to COM1. Clobbers AH and DX.
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

; Regions every map must contain: base, length, type (all below 4 GB)
req_regions:
    dd 0x00000000, 0x000A0000, 1    ; conventional memory
    dd 0x000A0000, 0x00060000, 2    ; VGA window, ROMs
    dd 0xFEC00000, 0x01400000, 2    ; IOAPIC, LAPIC, firmware
req_regions_end:

msg_entry     db 'OS-ENTRY x86', 0
msg_dl_ok     db ' dl=ok', 0
msg_dl_bad    db ' dl=bad', 0
msg_mm_ok     db ' memmap=ok', 0
msg_mm_bad    db ' memmap=bad', 0
msg_flags_ok  db ' flags=ok', 13, 10, 0
msg_flags_bad db ' flags=bad'
msg_crlf      db 13, 10, 0
msg_mm        db 'mm ', 0

saved_flags dw 0
saved_dx    dw 0
saved_di    dw 0

times 510 - ($ - $$) db 0
%ifdef NO_SIG
dw 0                            ; x86-bad-sector: a full sector, no signature
%else
dw 0xAA55
%endif
