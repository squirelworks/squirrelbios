; SquirrelBIOS x86 — Linux bzImage loader (32-bit boot protocol)
; Original code. Included by x86/squirrel-bios.asm.

;; Load a Linux bzImage (32-bit boot protocol, Documentation/x86/boot.rst):
; setup_sects at 0x1F1, HdrS already seen, syssize at 0x1F4 (16-byte
; units). The protected-mode kernel follows the boot and setup sectors and
; is copied to 0x100000 in BZ_CHUNK-sector pieces through a bounce buffer,
; so its size is bounded only by the RAM at 1 MB. boot_params gets the
; setup header, the E820 table, the command line and type_of_loader, and
; the kernel is entered in flat 32-bit protected mode at code32_start.
bzimage_go:
    movzx eax, byte [ss:BOOT_SECTOR + 0x1F1]
    test al, al
    jnz .sects
    mov al, 4
.sects:
    movzx ebx, al               ; setup_sects
    inc ebx                     ; boot + setup sectors before PM
    mov eax, [ss:BOOT_SECTOR + 0x1F4]
    test eax, eax
    jz .bad
    cmp eax, 0x0F000000         ; 3.75 GB in 16-byte units
    jae .toobig
    add eax, 31
    shr eax, 5                  ; 16-byte units -> sectors
    mov ecx, eax                ; PM sector count
    shl eax, 9                  ; bytes
    call bz_ram_at_1m           ; EDX = usable bytes from 0x100000
    cmp eax, edx
    ja .toobig

    call bz_fill_params

    mov eax, ebx                ; first PM LBA
    mov ebp, BZ_LOAD
.load_loop:
    mov edx, ecx                ; this chunk: min(left, BZ_CHUNK)
    cmp edx, BZ_CHUNK
    jbe .chunk
    mov edx, BZ_CHUNK
.chunk:
    push ecx
    push ebp
    mov cl, dl
    mov ebp, BZ_BOUNCE
    call ata_read_retry
    pop ebp
    pop ecx
    jc .readfail
    call bz_copy_chunk          ; EDX sectors to EBP; EBP advances
    add eax, edx
    sub ecx, edx
    jnz .load_loop

    call prepare_runtime
    mov si, msg_bzload
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

    cli
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, bz_gdt
    sub sp, 6
    mov bx, sp
    mov word [ss:bx], bz_gdt_end - bz_gdt - 1
    mov dword [ss:bx + 2], eax
    lgdt [ss:bx]
    add sp, 6
    mov eax, cr0
    or al, 1
    mov cr0, eax
    db 0x66, 0xEA
    dd bz_pm_entry + 0xF0000
    dw BZ_BOOT_CS

.bad:
    mov si, msg_bzbad
    jmp boot_fail
.toobig:
    mov si, msg_bzbig
    jmp boot_fail
.readfail:
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_bzread
    jmp disk_fail

; EDX = length of the usable SQMM entry that starts at 0x100000 (capped
; below 4 GB), or 0 if there is none. Other registers preserved.
bz_ram_at_1m:
    push cx
    push si
    xor edx, edx
    mov cx, [ss:MEMMAP_ADDR + 4]
    mov si, MEMMAP_ENTRIES
.next:
    jcxz .done
    cmp dword [ss:si], BZ_LOAD
    jne .skip
    cmp dword [ss:si + 4], 0
    jne .skip
    cmp dword [ss:si + 16], E820_USABLE
    jne .skip
    mov edx, [ss:si + 8]
    cmp dword [ss:si + 12], 0
    je .done
    mov edx, 0xFFFFFFFF - BZ_LOAD + 1   ; length past 4 GB: what fits below
    jmp .done
.skip:
    add si, MEMMAP_ENTRY
    dec cx
    jmp .next
.done:
    pop si
    pop cx
    ret

; Build boot_params (the "zero page") at BZ_BOOT_PARAMS: all zero, then
; the setup header from the image (0x1F1 up to 0x202 + byte at 0x201),
; then the loader's fields. Every register preserved.
bz_fill_params:
    pushad
    push ds
    push es
    mov ax, (BZ_BOOT_PARAMS >> 4)
    mov es, ax
    xor di, di
    xor eax, eax
    mov cx, 0x1000 / 4
    cld
    rep stosd

    ; Setup header
    movzx cx, byte [ss:BOOT_SECTOR + 0x201]
    add cx, 0x202 - 0x1F1
    cmp cx, BZ_HDR_MAX - 0x1F1
    jbe .hdrlen
    mov cx, BZ_HDR_MAX - 0x1F1
.hdrlen:
    xor ax, ax
    mov ds, ax
    mov si, BOOT_SECTOR + 0x1F1
    mov di, 0x1F1
    rep movsb

    ; Loader fields
    mov byte [es:0x210], BZ_LOADER_TYPE
    mov eax, [ss:BOOT_SECTOR + 0x214]
    test eax, eax
    jnz .entry
    mov eax, BZ_LOAD
.entry:
    mov [es:0x214], eax
    mov dword [es:0x218], 0         ; no initrd
    mov dword [es:0x21C], 0
    mov dword [es:0x228], BZ_CMDLINE

    ; E820 table: the SQMM entries are already in E820 layout
    mov cx, [ss:MEMMAP_ADDR + 4]
    cmp cx, BZ_E820_MAX
    jbe .count
    mov cx, BZ_E820_MAX
.count:
    mov [es:0x1E8], cl
    mov ax, MEMMAP_ENTRY / 2
    mul cx
    mov cx, ax
    mov si, MEMMAP_ENTRIES
    mov di, 0x2D0
    rep movsw

    ; alt_mem_k: KB of RAM above 1 MB
    call bz_ram_at_1m
    shr edx, 10
    mov [es:0x1E0], edx

    ; Command line, right after boot_params
    push cs
    pop ds
    mov si, bz_cmdline
    mov di, BZ_CMDLINE - BZ_BOOT_PARAMS
.cmd:
    lodsb
    stosb
    test al, al
    jnz .cmd

    pop es
    pop ds
    popad
    ret

; Copy EDX sectors from BZ_BOUNCE to linear EBP with unreal DS (flat);
; EBP advances past the copy. Do not reload DS in real mode in between:
; that would replace the flat cache with base = selector<<4.
bz_copy_chunk:
    push eax
    push ecx
    push esi
    push edi
    push ds
    call unreal_ds
    mov esi, BZ_BOUNCE
    mov edi, ebp
    mov ecx, edx
    shl ecx, 7                  ; sectors -> dwords
.copy:
    a32 mov eax, [esi]
    a32 mov [edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jnz .copy
    mov ebp, edi
    pop ds
    pop edi
    pop esi
    pop ecx
    pop eax
    ret

; Linux 32-bit entry: CS = 0x10 and DS/ES/SS = 0x18 (flat 4 GB), ESI =
; boot_params, EBP = EDI = EBX = 0, interrupts off.
BITS 32
bz_pm_entry:
    mov ax, BZ_BOOT_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, BZ_STACK
    cld
    mov esi, BZ_BOOT_PARAMS
    mov eax, [BZ_BOOT_PARAMS + 0x214]
    xor ebx, ebx
    xor ebp, ebp
    xor edi, edi
    jmp eax
BITS 16
