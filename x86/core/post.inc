; SquirrelBIOS x86 — BDA, A20, APIC virtual wire, unreal mode, GDTs, PIT and PICs
; Original code. Included by x86/squirrel-bios.asm.

bda_init:
    mov word [ss:BDA_EQUIP], BDA_EQUIP_VAL
    mov word [ss:BDA_MEMSIZE], BDA_MEM_KB
    mov word [ss:BDA_EBDA_SEG], EBDA_BASE >> 4
    ; EBDA: zeroed, its first byte its size in KB
    push es
    push di
    push cx
    push ax
    mov ax, EBDA_BASE >> 4
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, EBDA_SIZE / 2
    cld
    rep stosw
    mov byte [es:0], EBDA_SIZE >> 10
    pop ax
    pop cx
    pop di
    pop es
    mov word [ss:BDA_CURSOR], 0
    mov dword [ss:BDA_TICKS], 0
    mov byte [ss:BDA_TICK_OVR], 0
    mov word [ss:BDA_KBD_HEAD], BDA_KBD_BUF
    mov word [ss:BDA_KBD_TAIL], BDA_KBD_BUF
    ret

; Enable the A20 line through the fast gate at port 0x92 (System Control
; Port A). QEMU honours this. Bit 0 is RESET — leave it clear. Preserves
; everything except AL and the flags.
enable_a20:
    push ax
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    pop ax
    ret

; Put the local APIC in virtual-wire mode so the 8259's ExtINT reaches
; the CPU (QEMU's default CPU otherwise keeps IRQ0 from the PIT). Skips
; when CPUID reports no APIC. Uses a brief unreal-mode DS to touch
; 0xFEE00000. Clobbers EAX/EBX/ECX/EDX and transiently DS.
APIC_BASE       equ 0xFEE00000
APIC_SPIV       equ 0xF0
APIC_LVT0       equ 0x350
APIC_LVT1       equ 0x360

apic_virtual_wire:
    push eax
    push ebx
    push ecx
    push edx
    push ds
    pushf
    cli
    mov eax, 1
    cpuid
    test edx, 1 << 9
    jz .out
    ; Ensure the APIC is globally enabled at the usual base.
    mov ecx, 0x1B
    rdmsr
    or eax, 1 << 11
    and eax, 0xFFFFF000
    or eax, (APIC_BASE & 0xFFFFF000) | (1 << 11) | (1 << 8)
    xor edx, edx
    wrmsr
    call unreal_ds
    mov eax, APIC_BASE
    ; SPIV: software enable, spurious vector 0xFF
    mov dword [dword eax + APIC_SPIV], 0x1FF
    ; LVT0: ExtINT delivery, unmasked (virtual wire)
    mov dword [dword eax + APIC_LVT0], 0x700
    ; LVT1: NMI
    mov dword [dword eax + APIC_LVT1], 0x400
.out:
    popf
    pop ds
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; Load DS with a 4 GB flat data segment, then leave protected mode so the
; cached descriptor stays (unreal mode). GDT lives in this ROM; GDTR is
; built on the stack because the ROM is not writable.
unreal_ds:
    push eax
    push bx
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, apic_gdt
    sub sp, 6
    mov bx, sp
    mov word [ss:bx], apic_gdt_end - apic_gdt - 1
    mov dword [ss:bx + 2], eax
    lgdt [ss:bx]
    add sp, 6
    mov eax, cr0
    or al, 1
    mov cr0, eax
    mov ax, 0x08
    mov ds, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    pop bx
    pop eax
    ret

; GS = flat 4 GB data segment (base 0), through the ROM's GDT; the
; caller's GDTR is loaded again before returning. Interrupts must be off
; and stay off while GS is in use. Keeps every register but GS.
flat_gs:
    push eax
    push bx
    sub sp, 12
    mov bx, sp
    o32 sgdt [ss:bx + 6]
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, apic_gdt
    mov word [ss:bx], apic_gdt_end - apic_gdt - 1
    mov [ss:bx + 2], eax
    o32 lgdt [ss:bx]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    mov ax, 0x08
    mov gs, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    o32 lgdt [ss:bx + 6]
    add sp, 12
    pop bx
    pop eax
    ret

; Wait CX milliseconds (CX >= 1) on PIT channel 2: one 1 ms count in
; mode 0 per millisecond, gated through port 61h bit 0, done when OUT2
; (port 61h bit 5) goes high. The speaker stays off. Keeps every register.
PIT_CH2         equ 0x42
PIT_1MS         equ 1193            ; 1.193182 MHz input clock
delay_ms:
    push ax
    push cx
    push edx
.ms:
    in al, 0x61
    and al, 0xFC                ; gate low, speaker data off
    out 0x61, al
    mov al, 0xB0                ; channel 2, low then high byte, mode 0
    out PIT_CMD, al
    mov al, PIT_1MS & 0xFF
    out PIT_CH2, al
    mov al, PIT_1MS >> 8
    out PIT_CH2, al
    in al, 0x61
    or al, 0x01                 ; gate high: the count runs
    out 0x61, al
    mov edx, 0x00100000         ; give up on a PIT that never ends
.wait:
    in al, 0x61
    test al, 0x20
    jnz .done
    dec edx
    jnz .wait
.done:
    loop .ms
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    pop edx
    pop cx
    pop ax
    ret

align 8
apic_gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
apic_gdt_end:

%if CONFIG_BZIMAGE
align 8
bz_gdt:
    dq 0
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
bz_gdt_end:
%endif

; PIT channel 0 at ~18.2 Hz; both 8259s in the IBM PC AT layout.
; Master mask leaves IRQ0 open; everything else stays masked. IF is still
; clear, so no interrupt runs until the OS (or a stand-in) STIs.
pit_pic_init:
    push ax
    mov al, PIT_MODE
    out PIT_CMD, al
    xor al, al                  ; divisor 0 = 65536
    out PIT_CH0, al
    out PIT_CH0, al

    mov al, 0x11                ; ICW1: edge, cascade, ICW4 needed
    out PIC1_CMD, al
    out PIC2_CMD, al
    mov al, 0x08                ; ICW2 master: IRQ0 -> INT 08h
    out PIC1_DATA, al
    mov al, 0x70                ; ICW2 slave: IRQ8 -> INT 70h
    out PIC2_DATA, al
    mov al, 0x04                ; ICW3 master: slave on IRQ2
    out PIC1_DATA, al
    mov al, 0x02                ; ICW3 slave: cascade id 2
    out PIC2_DATA, al
    mov al, 0x01                ; ICW4: 8086 mode
    out PIC1_DATA, al
    out PIC2_DATA, al
    ; Keyboard: enable scan-code translation (set 2 -> set 1) and IRQ1,
    ; drain stale bytes, then unmask IRQ0 and IRQ1 on the PIC.
    call kbd_init
    mov al, 0xFC                ; OCW1: unmask IRQ0 (timer) and IRQ1 (kbd)
    out PIC1_DATA, al
    mov al, 0xFF
    out PIC2_DATA, al
    pop ax
    ret
