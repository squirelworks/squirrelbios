; SquirrelBIOS x86 — PCI BIOS 2.1, real mode (INT 1Ah AH = 0xB1)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_PCI.
;
; int1a_handler jumps here for AH = 0xB1, with nothing pushed:
;   AL = 0x01  installation check: AH = 0, AL = 0x01 (mechanism #1),
;              BX = 0x0210 (version 2.10), CL = last bus (0),
;              EDX = 'PCI '
;   AL = 0x02  find device CX (device ID), DX (vendor ID), SI = index
;   AL = 0x03  find class code ECX bits 23-0, SI = index
;              both: BH = bus, BL = device << 3 | function
;   AL = 0x08 / 0x09 / 0x0A  read config byte / word / dword:
;              BH, BL, DI = register; CL / CX / ECX = the value
;   AL = 0x0B / 0x0C / 0x0D  write config byte / word / dword, same
; CF clear and AH = 0 on success; otherwise CF set and AH = 0x81
; (function not supported), 0x83 (bad vendor ID), 0x86 (device not
; found) or 0x87 (bad register number). Registers that are not
; results are kept.

PCIB_OK         equ 0x00
PCIB_FUNC       equ 0x81
PCIB_VENDOR     equ 0x83
PCIB_NOTFOUND   equ 0x86
PCIB_REGISTER   equ 0x87

pci_int1a:
    push bp
    mov bp, sp                  ; [bp+6] = FLAGS
    call pci_present
    jc .func
    cmp al, 0x01
    je .check
    cmp al, 0x02
    je .find_dev
    cmp al, 0x03
    je .find_class
    cmp al, 0x08
    jb .func
    cmp al, 0x0D
    jbe .config
.func:
    mov ah, PCIB_FUNC
.fail:
    or word [bp + 6], 1
    pop bp
    iret
.ok:
    mov ah, PCIB_OK
    and word [bp + 6], 0xFFFE
    pop bp
    iret

.check:
    mov ax, 0x0001
    mov bx, 0x0210
    xor cl, cl
    mov edx, 0x20494350         ; 'PCI '
    jmp .ok

; Walk bus 0 to the SI'th function whose register 0 (vendor | device
; << 16) or register 8 without its revision byte (class << 8) equals the
; value built in ECX. DL = the register, DH = 1 to ignore the low byte.
.find_dev:
    cmp dx, 0xFFFF
    jne .find_dev_ok
    mov ah, PCIB_VENDOR
    jmp .fail
.find_dev_ok:
    push ecx
    push edx
    push edi
    push si
    shl ecx, 16
    mov cx, dx
    xor dx, dx
    jmp .find
.find_class:
    push ecx
    push edx
    push edi
    push si
    shl ecx, 8
    mov dx, 0x0108
.find:
    xor bx, bx
.find_next:
    call pci_next
    jc .not_found
    movzx di, dl
    call pci_read32
    test dh, dh
    jz .cmp
    xor al, al
.cmp:
    cmp eax, ecx
    jne .skip
    test si, si
    jz .found
    dec si
.skip:
    inc bl
    jnz .find_next
.not_found:
    pop si
    pop edi
    pop edx
    pop ecx
    mov ah, PCIB_NOTFOUND
    jmp .fail
.found:
    pop si
    pop edi
    pop edx
    pop ecx
    jmp .ok

.config:
    cmp di, 0xFF
    ja .badreg
    cmp al, 0x08
    je .rb
    cmp al, 0x0B
    je .wb
    test di, 1
    jnz .badreg
    cmp al, 0x09
    je .rw
    cmp al, 0x0C
    je .ww
    test di, 2
    jnz .badreg
    cmp al, 0x0A
    je .rd
.wd:
    call pci_write32
    jmp .ok
.wb:
    call pci_write8
    jmp .ok
.ww:
    call pci_write16
    jmp .ok
.rb:
    call pci_read8
    mov cl, al
    jmp .ok
.rw:
    call pci_read16
    mov cx, ax
    jmp .ok
.rd:
    call pci_read32
    mov ecx, eax
    jmp .ok
.badreg:
    mov ah, PCIB_REGISTER
    jmp .fail
