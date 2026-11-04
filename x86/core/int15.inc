; SquirrelBIOS x86 — INT 15h system services (E820, A20)
; Original code. Included by x86/squirrel-bios.asm.

; Stacked real-mode frame: [bp+2] = IP, [bp+4] = CS, [bp+6] = FLAGS.
; EAX=E820 memory map; AX=2400/2401/2402 A20 disable/enable/query via
; port 0x92 (same gate as enable_a20). AX=2403 reports port-0x92 support.
; AH=C2 is the pointing device interface, served by a USB mouse kept
; after POST (usb_mouse_int15, CONFIG_USB).
int15_handler:
    push bp
    mov bp, sp
    cmp eax, 0xE820
    je .e820
    cmp ax, 0x2400
    je .a20_off
    cmp ax, 0x2401
    je .a20_on
    cmp ax, 0x2402
    je .a20_query
    cmp ax, 0x2403
    je .a20_support
%if CONFIG_USB
    cmp ah, 0xC2
    je usb_mouse_int15
%endif
    jmp .fail

.a20_off:
    push ax
    in al, 0x92
    and al, 0xFC                ; clear A20 and RESET
    out 0x92, al
    pop ax
    xor ah, ah                  ; AH=0 success
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.a20_on:
    call enable_a20
    xor ah, ah
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.a20_query:
    in al, 0x92
    and al, 0x02
    shr al, 1                   ; AL = 0 disabled, 1 enabled
    xor ah, ah
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.a20_support:
    mov bx, 0x0002              ; bit 1: port 0x92 supported
    xor ah, ah
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.e820:
    cmp edx, 0x534D4150         ; 'SMAP'
    jne .fail
    cmp ecx, 20
    jb .fail
    test ebx, 0xFFFF0000
    jnz .fail
    push ds
    push si
    push di
    xor si, si
    mov ds, si
    cmp bx, [MEMMAP_ADDR + 4]   ; entry count
    jae .reject
    mov si, bx
    imul si, si, MEMMAP_ENTRY
    add si, MEMMAP_ENTRIES
    mov cx, MEMMAP_ENTRY / 2
    cld
    rep movsw
    pop di                      ; caller keeps its buffer pointer
    pop si
    inc bx
    cmp bx, [MEMMAP_ADDR + 4]
    jb .cont
    xor bx, bx                  ; last entry: continuation 0
.cont:
    pop ds
    mov eax, 0x534D4150
    mov ecx, 20
    and word [bp + 6], 0xFFFE   ; CF clear
    pop bp
    iret
.reject:
    pop di
    pop si
    pop ds
.fail:
    or word [bp + 6], 1         ; CF set
    pop bp
    iret

; ---------------------------------------------------------------------------
; ATA PIO
; ---------------------------------------------------------------------------
