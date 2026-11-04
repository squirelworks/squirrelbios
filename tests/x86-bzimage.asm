; Minimal Linux-style bzImage for the firmware's HdrS path.
; Original code.
;
; File layout:
;   0x000–0x1FF  boot sector: junk the loader must not copy, setup_sects,
;                syssize, AA55
;   0x200–0x3FF  setup: HdrS, version, loadflags, code32_start
;   0x400–…      32-bit payload (vstart 0x100000)
;
; The payload checks what the 32-bit boot protocol promises and prints
; one word per check on COM1:
;   bz=ok    it ran
;   seg=ok   CS = 0x10, DS = ES = SS = 0x18, EBX = EBP = EDI = 0
;   bp=ok    ESI = boot_params at 0x90000: setup header copied, the
;            boot-sector bytes below 0x1E0 are zero, type_of_loader = 0xFF,
;            an E820 table that starts with usable RAM at 0, alt_mem_k set,
;            and cmd_line_ptr pointing at "console=ttyS0"
;   tail=ok  the last dword of the payload arrived (with -DBIG the payload
;            is PAD_KB long, well past the old 64 KB limit)
; -DTOOBIG only claims a huge syssize; the firmware must refuse it before
; reading, so its payload never runs.

BITS 16

%ifdef BIG
PAD_KB equ 300
%else
PAD_KB equ 0
%endif

TAIL_MAGIC equ 0x4C494154       ; 'TAIL'

section hdr start=0
db 'JUNK'                       ; must not reach boot_params
times 0x1F1 - ($ - $$) db 0
db 1                            ; setup_sects
dw 0                            ; root_flags
%ifdef TOOBIG
dd (64 << 20) / 16              ; 64 MB, more than the test's RAM
%else
dd (pm_end - pm_start + 15) / 16
%endif
times 0x1FE - ($ - $$) db 0
dw 0xAA55

    jmp short hdr_end           ; 0x200: jump over the header
db 'HdrS'
dw 0x0204
times 0x211 - ($ - $$) db 0
db 0x01                         ; LOADED_HIGH
times 0x214 - ($ - $$) db 0
dd 0x00100000                   ; code32_start
times 0x226 - ($ - $$) db 0
hdr_end:
times 0x400 - ($ - $$) db 0

BITS 32
section pm start=0x400 vstart=0x100000
pm_start:
    cld
    mov [saved_esi], esi
    mov [saved_regs], ebx
    or [saved_regs], ebp
    or [saved_regs], edi
    mov esp, 0x90000

    mov esi, msg_bz
    call puts

    ; Segments and zeroed registers
    mov esi, msg_seg_bad
    mov ax, cs
    cmp ax, 0x10
    jne .seg_done
    mov ax, ds
    cmp ax, 0x18
    jne .seg_done
    mov ax, es
    cmp ax, 0x18
    jne .seg_done
    mov ax, ss
    cmp ax, 0x18
    jne .seg_done
    cmp dword [saved_regs], 0
    jne .seg_done
    mov esi, msg_seg_ok
.seg_done:
    call puts

    ; boot_params
    mov esi, msg_bp_bad
    mov ebx, [saved_esi]
    mov byte [bp_step], 'a'
    cmp ebx, 0x90000
    jne .bp_done
    mov byte [bp_step], 'b'
    mov ecx, 0x1E0              ; boot-sector bytes not copied (0x1E0+ are set)
    xor edi, edi
.zero:
    cmp byte [ebx + edi], 0
    jne .bp_done
    inc edi
    loop .zero
    mov byte [bp_step], 'c'
    cmp dword [ebx + 0x202], 'HdrS'
    jne .bp_done
    mov byte [bp_step], 'd'
    cmp byte [ebx + 0x1F1], 1   ; setup_sects copied
    jne .bp_done
    mov byte [bp_step], 'e'
    cmp byte [ebx + 0x210], 0xFF
    jne .bp_done
    mov byte [bp_step], 'f'
    cmp byte [ebx + 0x1E8], 3   ; at least low RAM, the hole, high RAM
    jb .bp_done
    mov byte [bp_step], 'g'
    cmp dword [ebx + 0x2D0], 0          ; e820_table[0].addr
    jne .bp_done
    mov byte [bp_step], 'h'
    cmp dword [ebx + 0x2D0 + 16], 1     ; e820_table[0].type usable
    jne .bp_done
    mov byte [bp_step], 'i'
    cmp dword [ebx + 0x1E0], 1024       ; alt_mem_k: at least 1 MB
    jb .bp_done
    mov byte [bp_step], 'j'
    mov edi, [ebx + 0x228]              ; cmd_line_ptr
    test edi, edi
    jz .bp_done
    push esi
    mov esi, want_cmd
    mov ecx, want_cmd_len
    repe cmpsb
    pop esi
    jne .bp_done
    mov esi, msg_bp_ok
.bp_done:
    call puts

    mov esi, msg_tail_bad
    cmp dword [tail], TAIL_MAGIC
    jne .tail_done
    mov esi, msg_tail_ok
.tail_done:
    call puts
    mov esi, msg_crlf
    call puts

.halt:
    hlt
    jmp .halt

puts:
    lodsb
    test al, al
    jz .ret
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    jmp puts
.ret:
    ret

saved_esi    dd 0
saved_regs   dd 0
want_cmd     db 'console=ttyS0'
want_cmd_len equ $ - want_cmd
msg_bz       db 'bz=ok ', 0
msg_seg_ok   db 'seg=ok ', 0
msg_seg_bad  db 'seg=bad ', 0
msg_bp_ok    db 'bp=ok ', 0
msg_bp_bad   db 'bp=bad:'
bp_step      db '?', ' ', 0
msg_tail_ok  db 'tail=ok', 0
msg_tail_bad db 'tail=bad', 0
msg_crlf     db 13, 10, 0

times PAD_KB * 1024 db 0x5A
align 4
tail dd TAIL_MAGIC
pm_end:
