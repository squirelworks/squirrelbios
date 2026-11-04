; SquirrelBIOS x86 — INT 1Ah time of day and RTC
; Original code. Included by x86/squirrel-bios.asm.

; INT 1Ah: AH=0x00 read ticks (CX:DX, AL = midnight flag then clear it);
; AH=0x02 read RTC time in BCD (CH=hours, CL=minutes, DH=seconds, DL=0).
; AH=0xB1 is the PCI BIOS (drivers/pci/int1apci.inc) when CONFIG_PCI.
; Other AH -> CF set. Stacked frame: [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS.
int1a_handler:
%if CONFIG_PCI
    cmp ah, 0xB1
    je pci_int1a
%endif
    push bp
    mov bp, sp
    cmp ah, 0x00
    je .ticks
    cmp ah, 0x02
    je .rtc
    or word [bp + 6], 1
    pop bp
    iret

.ticks:
    push ds
    xor ax, ax
    mov ds, ax
    mov dx, [BDA_TICKS]         ; low word
    mov cx, [BDA_TICKS + 2]     ; high word
    mov al, [BDA_TICK_OVR]
    mov byte [BDA_TICK_OVR], 0
    pop ds
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.rtc:
    push ax
    push bx
    ; Wait for Update-In-Progress (CMOS 0x0A bit 7) to clear.
    mov bx, 0x1000
.wait_uip:
    mov al, 0x0A
    call cmos_read
    test al, 0x80
    jz .read
    dec bx
    jnz .wait_uip
.read:
    mov al, 0x00                ; seconds
    call cmos_read
    mov dh, al
    mov al, 0x02                ; minutes
    call cmos_read
    mov cl, al
    mov al, 0x04                ; hours
    call cmos_read
    mov ch, al
    xor dl, dl                  ; daylight saving = 0
    pop bx
    pop ax
    mov ah, 0x02                ; keep function number
    and word [bp + 6], 0xFFFE
    pop bp
    iret
