; SquirrelBIOS x86_64-uefi: the last 4 KB of the flash image.
; Original code.
;
; The CPU starts in real mode at FFFFFFF0h. This stub enters 32-bit
; protected mode, copies the firmware body (the rest of the flash image,
; linked to run at FW_BASE) into RAM, builds page tables that map the
; first 4 GB one to one with 2 MB pages, enters 64-bit long mode and
; jumps to the body's entry point at FW_BASE.
;
; Built with nasm -f bin; FLASH_SIZE and FW_BASE come from the Makefile.

%ifndef FLASH_SIZE
%define FLASH_SIZE 0x100000
%endif
%ifndef FW_BASE
%define FW_BASE 0x200000
%endif

FLASH_BASE      equ 0x100000000 - FLASH_SIZE
STUB_SIZE       equ 0x1000
BODY_SIZE       equ FLASH_SIZE - STUB_SIZE

; Page tables, in conventional memory below the firmware
PML4            equ 0x1000
PDPT            equ 0x2000
PD0             equ 0x3000          ; four page directories, 0x3000-0x6FFF

SEL_CODE32      equ 0x08
SEL_DATA        equ 0x10
SEL_CODE64      equ 0x18

        org 0x100000000 - STUB_SIZE

; ---------------------------------------------------------------- 16-bit
        bits 16
start16:
        cli
        cld
        mov eax, cr0                    ; caches on, as after any reset
        and eax, ~0x60000000
        mov cr0, eax
        o32 lgdt [cs:gdt_ptr - $$ + 0x10000 - STUB_SIZE]
        mov eax, cr0
        or al, 1                        ; PE
        mov cr0, eax
        jmp dword SEL_CODE32:start32

; ---------------------------------------------------------------- 32-bit
        bits 32
start32:
        mov ax, SEL_DATA
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov fs, ax
        mov gs, ax

        ; Firmware body: flash to RAM
        mov esi, FLASH_BASE
        mov edi, FW_BASE
        mov ecx, BODY_SIZE / 4
        rep movsd

        ; Page tables: PML4[0] -> PDPT, PDPT[0..3] -> PD0..PD3,
        ; each PD 512 x 2 MB pages, present + writable + large
        mov edi, PML4
        mov ecx, (PD0 + 0x4000 - PML4) / 4
        xor eax, eax
        rep stosd
        mov dword [PML4], PDPT | 3
        mov dword [PDPT + 0x00], PD0 + 0x0000 | 3
        mov dword [PDPT + 0x08], PD0 + 0x1000 | 3
        mov dword [PDPT + 0x10], PD0 + 0x2000 | 3
        mov dword [PDPT + 0x18], PD0 + 0x3000 | 3
        mov edi, PD0
        mov eax, 0x83                   ; 0 MB, P | RW | PS
        xor edx, edx                    ; high half of the address
        mov ecx, 2048
.pde:
        mov [edi], eax
        mov [edi + 4], edx
        add edi, 8
        add eax, 0x200000
        adc edx, 0
        loop .pde

        mov eax, PML4
        mov cr3, eax
        mov eax, cr4
        or eax, 0x20 | 0x200 | 0x400    ; PAE, OSFXSR, OSXMMEXCPT
        mov cr4, eax
        mov ecx, 0xC0000080             ; EFER
        rdmsr
        or eax, 0x100                   ; LME
        wrmsr
        mov eax, cr0
        and eax, ~0x4                   ; EM off, MP on: SSE usable
        or eax, 0x80000002              ; PG | MP
        mov cr0, eax
        jmp SEL_CODE64:start64

; ---------------------------------------------------------------- 64-bit
        bits 64
start64:
        xor eax, eax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov fs, ax
        mov gs, ax
        mov rsp, FW_BASE                ; stack grows down from FW_BASE
        mov rax, FW_BASE
        jmp rax

; ---------------------------------------------------------------- GDT
        align 8
gdt:
        dq 0
        dq 0x00CF9A000000FFFF           ; 08h: 32-bit code, 4 GB
        dq 0x00CF92000000FFFF           ; 10h: data, 4 GB
        dq 0x00AF9A000000FFFF           ; 18h: 64-bit code
gdt_end:
gdt_ptr:
        dw gdt_end - gdt - 1
        dd gdt

; ---------------------------------------------------------------- reset
        times STUB_SIZE - 16 - ($ - $$) db 0xF4
reset:
        jmp start16                     ; FFFFFFF0h
        times STUB_SIZE - ($ - $$) db 0xF4
