; SquirrelBIOS x86 — IVT, default handler, IRQ0 timer
; Original code. Included by x86/squirrel-bios.asm.

install_ivt:
    push es
    push di
    push ax
    push cx
    xor ax, ax
    mov es, ax
    xor di, di
    cld
    mov cx, 256
.fill:
    mov ax, int_unhandled
    stosw
    mov ax, 0xF000
    stosw
    loop .fill
    mov word [es:0x08 * 4], irq0_handler
    mov word [es:0x09 * 4], irq1_handler
    mov word [es:0x10 * 4], int10_handler
    mov word [es:0x13 * 4], int13_handler
    mov word [es:0x15 * 4], int15_handler
    mov word [es:0x16 * 4], int16_handler
    mov word [es:0x1A * 4], int1a_handler
%if CONFIG_FLOPPY
    mov word [es:0x1E * 4], fd_param_table  ; data pointer, segment 0xF000
%endif
    pop cx
    pop ax
    pop di
    pop es
    ret

int_unhandled:
    iret

; IRQ0: PIT tick. Increments BDA_TICKS; wraps at a day and sets the
; overflow byte. EOI to the master PIC. Preserves all registers and the
; flags the interrupted code had on the stack.
irq0_handler:
    push ax
    push ds
    xor ax, ax
    mov ds, ax
    add dword [BDA_TICKS], 1
    cmp dword [BDA_TICKS], PIC_TICKS_DAY
    jb .eoi
    mov dword [BDA_TICKS], 0
    mov byte [BDA_TICK_OVR], 1
.eoi:
    mov al, 0x20
    out PIC1_CMD, al
    pop ds
    pop ax
    iret
