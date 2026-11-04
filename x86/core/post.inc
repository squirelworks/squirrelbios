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
