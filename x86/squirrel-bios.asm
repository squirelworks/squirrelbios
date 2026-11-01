; SquirrelBIOS — x86 firmware ROM for QEMU PC
; 64 KB image. Reset vector at offset 0xFFF0.
; Original code.
;
; Build:  nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
; Run:    qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide
;
; Behaviour:
;   - Set segments and stack
;   - Program COM1 (115200 8N1)
;   - Put the VGA in 80x25 text mode with an original font (vga.inc);
;     every message goes to VGA text and COM1
;   - Build a memory map at 0x0000:0x0500 (memmap.inc)
;   - Fill BDA equipment, conventional KB, and the teletype cursor
;   - SMBIOS 2.4 entry point at 0xFD000; ACPI 1.0 tables at 0xFE000
;   - Linux bzImage (HdrS): load PM kernel at 1 MB and enter it
;   - Scan option ROMs at 0xD0000–0xEFFFF (55 AA + checksum) and call init
;   - Enable the A20 gate (port 0x92) before the hand-off jump
;   - Program the PIT (~18.2 Hz) and both PICs (IRQ0/IRQ1)
;   - Enable the i8042 (set-1 XLAT) and INT 16h keyboard services
;   - INT 1Ah: ticks since midnight and RTC time (BCD)
;   - Print banner
;   - ATA PIO read of LBA 0 (sector 1) into 0x7C00, with timeouts,
;     status checks and retries (soft reset between attempts)
;   - On failure: print the reason and the ATA status/error registers, halt
;   - Check the 0xAA55 signature in the last word of the sector; halt if absent
;   - If the sector carries an SQBL header (offset 0x1F0), read the extra
;     sectors it asks for (LBA 1 on) to its load address, below 0xA0000
;   - If it does not, but LBA 192 ends in the Squirrel-OS/2 signature
;     'S2OK', read sectors 1..192 to 0x8000 and jump there. That boot
;     sector is not entered
;   - Otherwise jump to 0x0000:0x7C00 with DL = boot drive (0x80 master
;     or 0x81 slave), DI = 0x0500. When the primary master is absent the
;     firmware tries the primary slave. The IVT is installed either way:
;     INT 13h AH=00/02/03/08/42/43 read or write DL 0x80/0x81, INT 10h
;     AH=0E writes VGA text, INT 15h E820 returns the memory map. The
;     boot sector can load a kernel itself

BITS 16

; Firmware stack while it runs: the top of the memory map page, so a load
; anywhere outside 0x0500-0x05FF and 0x7C00-0x7DFF cannot overwrite it.
; The map ends by 0x0570 (5 entries) and the deepest call chain uses well
; under 64 bytes.
FW_STACK        equ 0x0600

; SQBL header at the end of sector 0 (x86-extra-sectors)
BOOT_SECTOR     equ 0x7C00
SQBL_HDR        equ BOOT_SECTOR + 0x1F0
SQBL_MAGIC      equ 0x4C425153  ; bytes 53 51 42 4C, 'SQBL'
SQBL_MAX        equ 128         ; extra sectors
LOAD_LIMIT      equ 0xA0000     ; first byte that is not conventional RAM

; Squirrel-OS/2 hard-disk image: 192 sectors (96 KB) at 0x8000, last
; dword 'S2OK' (documentation/kernel/boot-sector.md). LBA 0 is the boot
; sector, so the signature lives in LBA 192.
OS_LOAD         equ 0x8000
OS_SECTORS      equ 192
OS_SIG          equ 0x4B4F3253  ; bytes 53 32 4F 4B, 'S2OK'
OS_PROBE        equ 0x1000      ; scratch sector for the signature probe
OS_SIG_LIN      equ OS_LOAD + OS_SECTORS * 512 - 4
; Linux bzImage: PM kernel at 1 MB; bounce buffer for ATA (below 1 MB).
BZ_LOAD         equ 0x100000
BZ_BOUNCE       equ 0x8000
BZ_BOOT_PARAMS  equ 0x90000
BZ_MAX_SECTORS  equ 128

; Primary ATA channel, legacy ports
ATA_DATA        equ 0x1F0
ATA_ERROR       equ 0x1F1
ATA_COUNT       equ 0x1F2
ATA_LBA_LO      equ 0x1F3
ATA_LBA_MID     equ 0x1F4
ATA_LBA_HI      equ 0x1F5
ATA_DRIVE       equ 0x1F6
ATA_STATUS      equ 0x1F7       ; read: status, write: command
ATA_CTRL        equ 0x3F6       ; read: alt status, write: device control

ST_BSY          equ 0x80
ST_DRDY         equ 0x40
ST_DF           equ 0x20
ST_DRQ          equ 0x08
ST_ERR          equ 0x01

CTRL_NIEN       equ 0x02        ; no interrupts, we poll
CTRL_SRST       equ 0x04

CMD_READ_SECTORS  equ 0x20
CMD_WRITE_SECTORS equ 0x30

ATA_TIMEOUT     equ 0x00400000  ; status polls before giving up
ATA_ATTEMPTS    equ 3
; Drive select base (LBA bit set): master 0xE0, slave 0xF0. Boot DL is
; 0x80 or 0x81. Both live in the page below the memory map; 0x04FE is
; the ATA retry counter.
ATA_DEVSEL      equ 0x04FC
BOOT_DL         equ 0x04FD
ATA_RETRY_CNT   equ 0x04FE
ATA_SEL_MASTER  equ 0xE0
ATA_SEL_SLAVE   equ 0xF0

COM1            equ 0x3F8
UART_TIMEOUT    equ 0xFFFF      ; THR-empty polls per character

; Fixed geometry for INT 13h. The boot sector asks AH=08, then turns an
; LBA into CHS with those numbers, and AH=02 turns that CHS back into
; the same LBA. 16 heads and 63 sectors is the classic translation.
DISK_SPT        equ 63
DISK_HEADS      equ 16
; Max cylinder number 1023: CH=0xFF and CL bits 7-6 = 3, so CL = 0xFF.
DISK_CYL_LO     equ 0xFF
DISK_CYL_HI_CL  equ 0xFF

; BIOS data area (segment 0). 0x0500 is the memory map, so these stay
; below it. Filled by bda_init after the memory map and VGA are up.
BDA_EQUIP       equ 0x0410      ; equipment list word
BDA_MEMSIZE     equ 0x0413      ; conventional memory size, KB
BDA_CURSOR      equ 0x0450      ; column, then row, one byte each (page 0)
BDA_TICKS       equ 0x046C      ; dword: PIT ticks since midnight (IRQ0)
BDA_TICK_OVR    equ 0x0470      ; byte: 24-hour overflow flag
BDA_KBD_HEAD    equ 0x041A      ; keyboard buffer head (offset into segment 0)
BDA_KBD_TAIL    equ 0x041C      ; keyboard buffer tail
BDA_KBD_BUF     equ 0x041E      ; 16 words: ASCII, then scancode
BDA_KBD_END     equ 0x043E      ; first byte past the buffer
; Equipment: 80x25 colour (bits 4-5 = 10), one serial port (bits 9-11 = 1).
; No floppies, no parallel ports. Matches COM1 + VGA text in this firmware.
BDA_EQUIP_VAL   equ 0x0220
BDA_MEM_KB      equ 640         ; 0x00000-0x9FFFF, as in the memory map
; PIT channel 0: divisor 0 => 65536, ~18.2065 Hz from 1.193182 MHz
PIT_CMD         equ 0x43
PIT_CH0         equ 0x40
PIT_MODE        equ 0x36        ; ch0, lobyte/hibyte, mode 3, binary
; 8259s: IBM PC AT — IRQ0..7 at INT 08h..0Fh, IRQ8..15 at INT 70h..77h
PIC1_CMD        equ 0x20
PIC1_DATA       equ 0x21
PIC2_CMD        equ 0xA0
PIC2_DATA       equ 0xA1
PIC_TICKS_DAY   equ 0x1800B0    ; ticks in 24 hours at ~18.2 Hz
KBD_DATA        equ 0x60
KBD_STATUS      equ 0x64

; INT 13h status
ST_OK           equ 0x00
ST_BAD          equ 0x01        ; invalid function or parameter
ST_NOTFOUND     equ 0x04
ST_RESET        equ 0x05
ST_TIMEOUT      equ 0x80
ST_NOTREADY     equ 0xAA

VGA_SEG         equ 0xB800
VGA_LINE        equ 160         ; bytes per 80-column text row
ATTR_NORMAL     equ 0x0F
ATTR_ERROR      equ 0x0C

entry:
    cli
    cld
    ; Mask both PICs until pit_pic_init programs them for the hand-off.
    mov al, 0xFF
    out PIC1_DATA, al
    out PIC2_DATA, al
    mov ax, cs                  ; strings live in the ROM segment
    mov ds, ax
    xor ax, ax
    mov ss, ax
    mov sp, FW_STACK
    mov ax, VGA_SEG
    mov es, ax

    call build_memmap
    call serial_init
    call vga_init
    call bda_init
    call smbios_init
    call acpi_init

    ; Banner
    xor di, di
    mov si, banner
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

    ; Disk: sector 0 (LBA 0) to 0x7C00. Prefer the primary master; if
    ; nothing answered as master, try the primary slave and keep DL.
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [ss:BOOT_DL], 0x80
    xor eax, eax
    mov cl, 1
    mov ebp, BOOT_SECTOR
    call ata_read_retry
    jnc .got_boot
    cmp si, msg_nodrive
    jne disk_fail
    mov byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    mov byte [ss:BOOT_DL], 0x81
    xor eax, eax
    mov cl, 1
    mov ebp, BOOT_SECTOR
    call ata_read_retry
    jc disk_fail
.got_boot:

; A clean read without the signature is not a boot sector. Reading it
; again would give the same bytes, so this is not retried.
    cmp word [ss:BOOT_SECTOR + 0x1FE], 0xAA55    ; SS = 0
    je .signed
    mov si, msg_nosig
    jmp boot_fail
.signed:

    ; No SQBL header: either a Squirrel-OS/2 image or one sector
    cmp dword [ss:SQBL_HDR], SQBL_MAGIC
    jne maybe_os

    ; Extra sector count, 1-128
    movzx ecx, word [ss:SQBL_HDR + 4]
    test cx, cx
    jz .range
    cmp cx, SQBL_MAX
    ja .range
    ; EAX = segment * 16 + offset, EDX = end of the load
    movzx eax, word [ss:SQBL_HDR + 6]
    shl eax, 4
    movzx edx, word [ss:SQBL_HDR + 8]
    add eax, edx
    cmp eax, LOAD_LIMIT
    jae .range
    mov edx, ecx
    shl edx, 9
    add edx, eax
    cmp edx, LOAD_LIMIT
    ja .range
    ; [EAX, EDX) must miss the memory map page and the boot sector
    cmp eax, MEMMAP_ADDR + 0x100
    jae .past_map
    cmp edx, MEMMAP_ADDR
    ja .overlap
.past_map:
    cmp eax, BOOT_SECTOR + 0x200
    jae .load
    cmp edx, BOOT_SECTOR
    ja .overlap
.load:
    mov ebp, eax
    mov eax, 1                  ; from LBA 1; CL is still the sector count
    call ata_read_retry
    jnc handoff
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_readerr_extra
    jmp disk_fail
.range:
    mov si, msg_range
    jmp boot_fail
.overlap:
    mov si, msg_overlap
    jmp boot_fail

; Linux bzImage (HdrS at 0x202) or a 96 KB 'S2OK' image, else one sector.
; HdrS lives in sector 1; setup_sects is in sector 0 at 0x1F1.
maybe_os:
    mov eax, 1
    mov cl, 1
    mov ebp, BOOT_SECTOR + 512
    call ata_read
    jnc .have_sect1
    call ata_reset
    jmp maybe_s2ok
.have_sect1:
    cmp dword [ss:BOOT_SECTOR + 0x202], 'HdrS'
    je bzimage_go

maybe_s2ok:
    mov eax, OS_SECTORS         ; LBA of the last kernel sector
    mov cl, 1
    mov ebp, OS_PROBE
    call ata_read
    jnc .probed
    call ata_reset              ; do not leave ERR set for the boot sector
    jmp handoff
.probed:
    cmp dword [ss:OS_PROBE + 508], OS_SIG
    jne handoff

    mov eax, 1
    mov cl, 128                 ; LBAs 1..128 -> 0x8000
    mov ebp, OS_LOAD
    call ata_read_retry
    jc kernel_fail
    mov eax, 129
    mov cl, OS_SECTORS - 128    ; LBAs 129..192 -> 0x18000
    mov ebp, OS_LOAD + 128 * 512
    call ata_read_retry
    jc kernel_fail
    push es
    mov ax, OS_SIG_LIN >> 4
    mov es, ax
    cmp dword [es:OS_SIG_LIN & 0x0F], OS_SIG
    pop es
    je kernel_go
    mov si, msg_nosig_os
    jmp boot_fail
kernel_fail:
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_kernel
    jmp disk_fail
kernel_go:
    call prepare_runtime
    mov si, msg_kload
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

; Registers match the 0x7C00 hand-off. CS:IP is the kernel.
kernel_entry:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
    mov dl, [ss:BOOT_DL]
    mov di, MEMMAP_ADDR
    jmp 0x0000:OS_LOAD

;; Load a Linux bzImage: setup_sects at 0x1F1, HdrS already seen, syssize
; at 0x1F4 (paragraphs). Copy the PM image to 0x100000 through a bounce
; buffer, fill a tiny boot_params, then enter 32-bit protected mode at
; code32_start (dword at 0x214, default 0x100000).
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
    add eax, 31
    shr eax, 5                  ; paragraphs -> sectors
    cmp eax, BZ_MAX_SECTORS
    ja .bad
    test eax, eax
    jz .bad
    mov ecx, eax                ; PM sector count
    push es
    mov ax, (BZ_BOOT_PARAMS >> 4)
    mov es, ax
    mov eax, [ss:BOOT_SECTOR + 0x214]
    test eax, eax
    jnz .entry
    mov eax, BZ_LOAD
.entry:
    mov [es:0x214], eax
    ; Zero boot_params and copy the first 0x250 header bytes into it
    push di
    push cx
    xor di, di
    xor eax, eax
    mov cx, 0x1000 / 4
    rep stosd
    push ds
    mov ax, (BOOT_SECTOR >> 4)
    mov ds, ax
    xor si, si
    xor di, di
    mov cx, 0x250 / 2
    rep movsw
    pop ds
    pop cx
    pop di
    pop es

    mov eax, ebx                ; first PM LBA
    mov ebp, BZ_LOAD
.load_loop:
    push eax
    push ecx
    push ebp
    mov cl, 1
    mov ebp, BZ_BOUNCE
    call ata_read_retry
    pop ebp
    pop ecx
    pop eax
    jc .readfail
    call bz_copy_sector
    inc eax
    dec ecx
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
    dw 0x08

.bad:
    mov si, msg_bzbad
    jmp boot_fail
.readfail:
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_bzread
    jmp disk_fail

; Copy 512 bytes BZ_BOUNCE -> linear EBP with unreal DS (flat); EBP += 512.
; Do not reload ES/DS in real mode — that would replace the flat cache
; with base = selector<<4.
bz_copy_sector:
    push eax
    push ecx
    push esi
    push edi
    push ds
    call unreal_ds
    mov esi, BZ_BOUNCE
    mov edi, ebp
    mov ecx, 512 / 4
.copy:
    a32 mov eax, [esi]
    a32 mov [edi], eax
    add esi, 4
    add edi, 4
    loop .copy
    mov ebp, edi
    pop ds
    pop edi
    pop esi
    pop ecx
    pop eax
    ret

BITS 32
bz_pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, BZ_LOAD
    mov esi, BZ_BOOT_PARAMS
    mov eax, [BZ_BOOT_PARAMS + 0x214]
    jmp eax
BITS 16

handoff:
    call prepare_runtime
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
    mov dl, [ss:BOOT_DL]        ; 0x80 master or 0x81 slave
    mov di, MEMMAP_ADDR         ; memory map header
    jmp 0x0000:0x7C00

; A20, APIC virtual-wire, PIT, PIC, IVT. IF stays clear; IRQ0 is unmasked
; so a later STI sees the timer. Called once on every hand-off path.
prepare_runtime:
    call enable_a20
    call apic_virtual_wire
    call pit_pic_init
    call install_ivt
    call optrom_scan
    ret

; Scan legacy option ROM space 0xD0000–0xEFFFF (skip VGA at 0xC0000).
; Each 2 KB: 0x55 0xAA, size byte (512-byte units), checksum 0 over the
; ROM, then far-call offset 3. Bad signatures or checksums are skipped.
OPTROM_SEG_FIRST equ 0xD000
OPTROM_SEG_LAST  equ 0xF000          ; stop before the system BIOS

optrom_scan:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds
    mov ax, OPTROM_SEG_FIRST
.next:
    cmp ax, OPTROM_SEG_LAST
    jae .done
    mov es, ax
    cmp word [es:0], 0xAA55
    jne .advance
    movzx cx, byte [es:2]       ; size in 512-byte units
    test cx, cx
    jz .advance
    cmp cx, 128                 ; at most 64 KB
    ja .advance
    ; Checksum over cx*512 bytes
    push ax
    xor dl, dl
    xor bx, bx
    mov ax, cx
    shl ax, 9                   ; byte count
    mov si, ax
.sum:
    add dl, [es:bx]
    inc bx
    cmp bx, si
    jb .sum
    pop ax
    test dl, dl
    jnz .advance
    ; Far call ES:0003. Preserve our DS; the ROM may clobber AX..DX.
    push ax
    pushf
    mov bx, 3
    push es
    push bx
    mov bx, sp
    call far [ss:bx]
    add sp, 4
    popf
    pop ax
.advance:
    ; Next 2 KB boundary: size may be larger; skip by max(2KB, size*512)
    movzx bx, byte [es:2]
    test bx, bx
    jz .step2k
    cmp word [es:0], 0xAA55
    jne .step2k
    shl bx, 5                   ; size*512 / 16 = size*32 paragraphs
    cmp bx, 0x80                ; at least 2 KB = 0x80 paras
    jae .skip
.step2k:
    mov bx, 0x80
.skip:
    add ax, bx
    jmp .next
.done:
    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; In: SI = reason, BL = ATA status, BH = ATA error register
disk_fail:
    mov ah, ATTR_ERROR
    call con_print
    mov si, msg_status
    call con_print
    mov al, bl
    call con_hex8
    mov si, msg_error
    call con_print
    mov al, bh
    call con_hex8
    jmp boot_halt

; In: SI = reason.  A failure that is not the drive's: no ATA registers.
boot_fail:
    mov ah, ATTR_ERROR
    call con_print
; Ends the line in progress, prints 'boot halted' and halts.
boot_halt:
    call con_newline
    mov si, msg_halted
    call con_print
    call con_newline
.halt:
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; BIOS data area. Called after build_memmap and vga_init. SS is already 0.
; INT 10h AH=0E keeps the cursor word at BDA_CURSOR in range for 80x25.
; ---------------------------------------------------------------------------

bda_init:
    mov word [ss:BDA_EQUIP], BDA_EQUIP_VAL
    mov word [ss:BDA_MEMSIZE], BDA_MEM_KB
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

align 8
bz_gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
bz_gdt_end:

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

; Program the i8042 so the OS sees XT (set-1) make/break codes: write
; the controller command byte with XLAT and keyboard IRQ enabled, enable
; scanning, and discard anything already in the output buffer.
kbd_init:
    push ax
    push cx
    mov al, 0x60                ; write command byte
    call kbd_cmd
    mov al, 0x65                ; IRQ1 + SYS + XLAT
    call kbd_data
    mov al, 0xAE                ; enable keyboard interface
    call kbd_cmd
    call kbd_drain
    pop cx
    pop ax
    ret

kbd_drain:
    mov cx, 32
.poll:
    in al, KBD_STATUS
    test al, 1
    jz .done
    in al, KBD_DATA
    loop .poll
.done:
    ret

; Wait for the input buffer to empty, then write AL to the command port.
kbd_cmd:
    call kbd_wait_in
    out KBD_STATUS, al
    ret

; Wait for the input buffer to empty, then write AL to the data port.
kbd_data:
    call kbd_wait_in
    out KBD_DATA, al
    ret

kbd_wait_in:
    push ax
    push cx
    mov cx, 0xFFFF
.wait:
    in al, KBD_STATUS
    test al, 2                  ; input buffer full
    jz .ok
    loop .wait
.ok:
    pop cx
    pop ax
    ret

; ---------------------------------------------------------------------------
; Real-mode IVT, installed before every hand-off. The ROM is mapped at
; 0xF0000, which is the segment stored in the IVT.
;
;   INT 10h AH=0x0E   teletype to VGA text (the boot sector's failure
;                     line, and the kernel's old-CPU line)
;   INT 13h AH=0x00   reset the primary master
;   INT 13h AH=0x02   CHS read
;   INT 13h AH=0x03   CHS write
;   INT 13h AH=0x08   drive parameters (16 heads, 63 sectors)
;   INT 13h AH=0x42   extended read (LBA), packet size 0x10
;   INT 13h AH=0x43   extended write (LBA), packet size 0x10
;   INT 15h EAX=E820  next SQMM entry, same table as 0x0500
;   INT 16h AH=0x00   read keystroke (wait)
;   INT 16h AH=0x01   check keystroke (ZF clear if ready)
;   INT 1Ah AH=0x00   read tick count since midnight
;   INT 1Ah AH=0x02   read RTC time (BCD)
;
; Any other request on those vectors returns with CF set (INT 16h check
; clears CF and uses ZF). IRQ0 increments the BDA tick count; IRQ1 feeds
; the keyboard buffer. Every other vector just irets.
; ---------------------------------------------------------------------------

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

; IRQ1: keyboard. Read the scancode, ignore break codes, enqueue a
; make-code pair (ASCII, scancode) in the BDA ring buffer when there is
; room. EOI to the master PIC.
irq1_handler:
    push ax
    push bx
    push ds
    xor ax, ax
    mov ds, ax
    in al, KBD_DATA
    test al, 0x80
    jnz .eoi                     ; break code
    call kbd_lookup
    jc .eoi                      ; unknown make code: drop
    ; AL = ASCII, AH = scancode. Enqueue at tail if not full.
    mov bx, [BDA_KBD_TAIL]
    mov [bx], ax
    add bx, 2
    cmp bx, BDA_KBD_END
    jb .wrap
    mov bx, BDA_KBD_BUF
.wrap:
    cmp bx, [BDA_KBD_HEAD]
    je .eoi                      ; full: drop the key
    mov [BDA_KBD_TAIL], bx
.eoi:
    mov al, 0x20
    out PIC1_CMD, al
    pop ds
    pop bx
    pop ax
    iret

; In: AL = make scancode. Out: CF clear, AL = ASCII, AH = scancode;
; CF set if this firmware has no ASCII for it. Uses CS-relative table.
kbd_lookup:
    push bx
    push ds
    mov ah, al                  ; keep scancode
    cmp al, kbd_map_len
    jae .bad
    mov bx, cs
    mov ds, bx
    mov bx, kbd_map
    xlat                        ; AL = map[scancode]
    test al, al
    jz .bad
    ; AL = ASCII, AH = scancode
    pop ds
    pop bx
    clc
    ret
.bad:
    pop ds
    pop bx
    stc
    ret

; Set-1 make codes -> ASCII. Index is the scancode; 0 means "no glyph".
; Only the keys the stand-in injects need to work; a few letters and
; digits are enough for a PC-compatible INT 16h surface.
kbd_map:
    db 0, 0, '1', '2', '3', '4', '5', '6'      ; 00-07
    db '7', '8', '9', '0', '-', '=', 8, 9      ; 08-0F  BS TAB
    db 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i'  ; 10-17
    db 'o', 'p', '[', ']', 13, 0, 'a', 's'     ; 18-1F  Enter
    db 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';'  ; 20-27
    db "'", '`', 0, '\', 'z', 'x', 'c', 'v'    ; 28-2F
    db 'b', 'n', 'm', ',', '.', '/', 0, '*'    ; 30-37
    db 0, ' '                                   ; 38-39
kbd_map_len equ $ - kbd_map

; INT 16h: AH=0x00 read (wait), AH=0x01 check (ZF). Other AH -> CF set.
; Stacked frame: [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS.
int16_handler:
    push bp
    mov bp, sp
    cmp ah, 0x00
    je .read
    cmp ah, 0x01
    je .check
    or word [bp + 6], 1         ; CF
    pop bp
    iret
.read:
    push ds
    push bx
    xor bx, bx
    mov ds, bx
.wait:
    mov bx, [BDA_KBD_HEAD]
    cmp bx, [BDA_KBD_TAIL]
    jne .got
    sti
    hlt
    cli
    jmp .wait
.got:
    mov ax, [bx]
    add bx, 2
    cmp bx, BDA_KBD_END
    jb .rh
    mov bx, BDA_KBD_BUF
.rh:
    mov [BDA_KBD_HEAD], bx
    pop bx
    pop ds
    and word [bp + 6], 0xFFFE   ; clear CF
    ; ZF not meaningful for AH=00
    pop bp
    iret
.check:
    push ds
    push bx
    xor bx, bx
    mov ds, bx
    mov bx, [BDA_KBD_HEAD]
    cmp bx, [BDA_KBD_TAIL]
    je .empty
    mov ax, [bx]                ; peek; do not advance head
    pop bx
    pop ds
    and word [bp + 6], 0xFFBE   ; clear CF and ZF
    pop bp
    iret
.empty:
    pop bx
    pop ds
    and word [bp + 6], 0xFFFE   ; clear CF
    or word [bp + 6], 0x0040    ; set ZF
    pop bp
    iret

; --- end keyboard --------------------------------------------------------

; INT 1Ah: AH=0x00 read ticks (CX:DX, AL = midnight flag then clear it);
; AH=0x02 read RTC time in BCD (CH=hours, CL=minutes, DH=seconds, DL=0).
; Other AH -> CF set. Stacked frame: [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS.
int1a_handler:
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


; Stacked real-mode frame: [bp+2] = IP, [bp+4] = CS, [bp+6] = FLAGS.
; AH=0x02 set cursor, 0x03 get cursor, 0x06/0x07 scroll, 0x0E teletype,
; 0x0F get mode. Page BH is ignored (only page 0). Other AH -> CF set.
int10_handler:
    push bp
    mov bp, sp
    cmp ah, 0x0E
    je .tele
    cmp ah, 0x02
    je .setcur
    cmp ah, 0x03
    je .getcur
    cmp ah, 0x06
    je .scroll
    cmp ah, 0x07
    je .scroll
    cmp ah, 0x0F
    je .getmode
    or word [bp + 6], 1
    pop bp
    iret
.tele:
    call tty_putc
    jmp .ok
.setcur:
    call vid_set_cursor
    jmp .ok
.getcur:
    call vid_get_cursor
    jmp .ok
.getmode:
    mov ah, VGA_COLS
    mov al, 3
    xor bh, bh
    jmp .ok
.scroll:
    call vid_scroll
.ok:
    and word [bp + 6], 0xFFFE
    pop bp
    iret

; DH=row, DL=col. Clamps to 80x25, stores BDA_CURSOR, updates CRTC.
vid_set_cursor:
    push ds
    push ax
    push bx
    xor ax, ax
    mov ds, ax
    mov bl, dl
    cmp bl, VGA_COLS
    jb .c
    mov bl, VGA_COLS - 1
.c:
    mov bh, dh
    cmp bh, VGA_ROWS
    jb .r
    mov bh, VGA_ROWS - 1
.r:
    mov [BDA_CURSOR], bx
    call tty_hwcursor
    pop bx
    pop ax
    pop ds
    ret

; Out: DH=row, DL=col, CH/CL = cursor type, BH = 0.
vid_get_cursor:
    push ds
    push ax
    xor ax, ax
    mov ds, ax
    mov ax, [BDA_CURSOR]
    mov dl, al
    mov dh, ah
    mov cx, 0x0607
    xor bh, bh
    pop ax
    pop ds
    ret

; AH=06 scroll up / 07 scroll down. AL = line count (0 = blank window).
; BH = attribute for blank rows. CH/CL = upper-left row/col,
; DH/DL = lower-right row/col.
;
; Full-screen one-line scrolls use the fast path; any other request
; blanks the window (AL=0 behaviour). Locals live on the stack.
vid_scroll:
    push bp
    mov bp, sp
    sub sp, 8
    ; [bp-1]=fn AH, [bp-2]=lines AL, [bp-3]=attr BH,
    ; [bp-4]=r0 CH, [bp-5]=c0 CL, [bp-6]=r1 DH, [bp-7]=c1 DL
    mov [bp - 1], ah
    mov [bp - 2], al
    mov [bp - 3], bh
    mov [bp - 4], ch
    mov [bp - 5], cl
    mov [bp - 6], dh
    mov [bp - 7], dl
    push ds
    push es
    pusha
    mov ax, VGA_SEG
    mov es, ax
    cmp byte [bp - 5], VGA_COLS
    jb .ok0
    mov byte [bp - 5], VGA_COLS - 1
.ok0:
    cmp byte [bp - 7], VGA_COLS
    jb .ok1
    mov byte [bp - 7], VGA_COLS - 1
.ok1:
    cmp byte [bp - 4], VGA_ROWS
    jb .ok2
    mov byte [bp - 4], VGA_ROWS - 1
.ok2:
    cmp byte [bp - 6], VGA_ROWS
    jb .ok3
    mov byte [bp - 6], VGA_ROWS - 1
.ok3:
    mov al, [bp - 4]
    cmp al, [bp - 6]
    ja .done
    mov al, [bp - 5]
    cmp al, [bp - 7]
    ja .done
    cmp byte [bp - 2], 1
    jne .blank
    cmp byte [bp - 4], 0
    jne .blank
    cmp byte [bp - 5], 0
    jne .blank
    cmp byte [bp - 6], VGA_ROWS - 1
    jne .blank
    cmp byte [bp - 7], VGA_COLS - 1
    jne .blank
    cmp byte [bp - 1], 0x06
    je .up
    mov ax, VGA_SEG
    mov ds, ax
    std
    mov si, VGA_LINE * (VGA_ROWS - 1) - 2
    mov di, VGA_LINE * VGA_ROWS - 2
    mov cx, VGA_COLS * (VGA_ROWS - 1)
    rep movsw
    cld
    mov ah, [bp - 3]
    mov al, ' '
    xor di, di
    mov cx, VGA_COLS
    rep stosw
    jmp .done
.up:
    call tty_scroll
    mov ah, [bp - 3]
    cmp ah, ATTR_NORMAL
    je .done
    mov al, ' '
    mov di, VGA_LINE * (VGA_ROWS - 1)
    mov cx, VGA_COLS
    rep stosw
    jmp .done
.blank:
    mov ah, [bp - 3]
    mov al, ' '
    mov bl, [bp - 4]
.brow:
    cmp bl, [bp - 6]
    ja .done
    movzx di, bl
    imul di, di, VGA_LINE
    movzx cx, byte [bp - 5]
    add di, cx
    add di, cx
    movzx cx, byte [bp - 7]
    movzx dx, byte [bp - 5]
    sub cx, dx
    inc cx
    push ax
    rep stosw
    pop ax
    inc bl
    jmp .brow
.done:
    popa
    pop es
    pop ds
    mov sp, bp
    pop bp
    ret

; Teletype. AL = character. CR, LF, BS and BEL are the usual ones;
; everything else is written in the firmware's normal attribute and
; the cursor advances, wrapping and scrolling at the bottom.
tty_putc:
    pushad
    push ds
    push es
    xor dx, dx
    mov ds, dx
    mov bx, [BDA_CURSOR]        ; BL = column, BH = row
    cmp al, 0x0D
    je .cr
    cmp al, 0x0A
    je .lf
    cmp al, 0x08
    je .bs
    cmp al, 0x07
    je .done
    movzx di, bh
    imul di, di, VGA_LINE
    movzx cx, bl
    shl cx, 1
    add di, cx
    mov ah, ATTR_NORMAL
    mov cx, VGA_SEG
    mov es, cx
    mov [es:di], ax
    inc bl
    cmp bl, VGA_COLS
    jb .store
    xor bl, bl
    jmp .lf
.cr:
    xor bl, bl
    jmp .store
.bs:
    test bl, bl
    jz .store
    dec bl
    jmp .store
.lf:
    inc bh
    cmp bh, VGA_ROWS
    jb .store
    call tty_scroll
    mov bh, VGA_ROWS - 1
.store:
    mov [BDA_CURSOR], bx
    call tty_hwcursor
.done:
    pop es
    pop ds
    popad
    ret

tty_scroll:
    push ds
    push es
    push ax
    push cx
    push si
    push di
    mov ax, VGA_SEG
    mov ds, ax
    mov es, ax
    mov si, VGA_LINE
    xor di, di
    mov cx, VGA_COLS * (VGA_ROWS - 1)
    cld
    rep movsw
    mov ax, (ATTR_NORMAL << 8) | ' '
    mov cx, VGA_COLS
    rep stosw
    pop di
    pop si
    pop cx
    pop ax
    pop es
    pop ds
    ret

; BL = column, BH = row. Programs the CRTC cursor.
tty_hwcursor:
    push ax
    push cx
    push dx
    movzx ax, bh
    mov cx, VGA_COLS
    mul cx
    movzx cx, bl
    add ax, cx
    mov cx, ax
    mov dx, VGA_CRTC_I
    mov al, 0x0E
    out dx, al
    inc dx
    mov al, ch
    out dx, al
    dec dx
    mov al, 0x0F
    out dx, al
    inc dx
    mov al, cl
    out dx, al
    pop dx
    pop cx
    pop ax
    ret

; INT 13h. pushad frame, SP = BP:
;   [bp+28] EAX (AL at +28, AH at +29)
;   [bp+24] ECX (CL at +24, CH at +25)
;   [bp+20] EDX (DL at +20, DH at +21)
;   [bp+16] EBX
;   [bp+36] FLAGS
; Primary master (DL = 0x80) and primary slave (DL = 0x81) are drives.
; CF follows the status in AH: clear on success, set on failure.
int13_handler:
    pushad
    mov bp, sp
    push ds
    push es
    cmp ah, 0x00
    je .reset
    cmp ah, 0x02
    je .read_chs
    cmp ah, 0x03
    je .write_chs
    cmp ah, 0x08
    je .params
    cmp ah, 0x42
    je .read_ext
    cmp ah, 0x43
    je .write_ext
    jmp .invalid

.reset:
    call disk_set_dev
    jc .invalid
    call ata_reset
    jc .reset_fail
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out
.reset_fail:
    mov byte [bp + 29], ST_RESET
    or word [bp + 36], 1
    jmp .out

.params:
    call disk_set_dev
    jc .invalid
    mov byte [bp + 29], ST_OK
    mov byte [bp + 25], DISK_CYL_LO
    mov byte [bp + 24], DISK_CYL_HI_CL
    mov byte [bp + 21], DISK_HEADS - 1
    mov byte [bp + 20], 1           ; one hard disk reported
    and word [bp + 36], 0xFFFE
    jmp .out

.read_ext:
    call disk_parse_packet
    jc .invalid
    push bp
    mov ebp, edx
    call disk_clear_stale
    call ata_read                   ; EAX = LBA, CL = count, EBP = dest
    pop bp
    jc .ioerr
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.write_ext:
    call disk_parse_packet
    jc .invalid
    push bp
    mov ebp, edx
    call disk_clear_stale
    call ata_write
    pop bp
    jc .ioerr
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.read_chs:
    call disk_set_dev
    jc .invalid
    movzx esi, byte [bp + 28]       ; AL = count
    test si, si
    jz .invalid
    cmp si, 128
    ja .invalid
    movzx eax, byte [bp + 24]       ; CL
    and al, 0x3F
    jz .notfound
    cmp al, DISK_SPT
    ja .notfound
    movzx ebx, al                   ; sector (1-63)
    movzx eax, byte [bp + 24]
    shr ax, 6
    and ax, 3
    shl ax, 8
    movzx edx, byte [bp + 25]       ; CH
    or eax, edx                     ; cylinder
    movzx edx, byte [bp + 21]       ; DH = head
    cmp dx, DISK_HEADS
    jae .notfound
    push ebx                        ; sector
    push edx                        ; head
    mov ebx, DISK_HEADS
    mul ebx
    pop edx
    add eax, edx
    mov ebx, DISK_SPT
    mul ebx
    pop edx                         ; sector
    dec edx
    add eax, edx                    ; LBA
    movzx edx, word [bp + 16]       ; BX = buffer offset
    mov bx, es
    movzx ebx, bx
    shl ebx, 4
    add edx, ebx                    ; linear destination
    mov ebx, esi
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .invalid
    cmp ebx, LOAD_LIMIT
    ja .invalid
    push bp
    mov ebp, edx
    mov ecx, esi                    ; CL = count
    call disk_clear_stale
    call ata_read
    pop bp
    jc .ioerr_chs
    mov byte [bp + 29], ST_OK       ; AL stays the count we transferred
    and word [bp + 36], 0xFFFE
    jmp .out

.write_chs:
    call disk_set_dev
    jc .invalid
    movzx esi, byte [bp + 28]
    test si, si
    jz .invalid
    cmp si, 128
    ja .invalid
    movzx eax, byte [bp + 24]
    and al, 0x3F
    jz .notfound
    cmp al, DISK_SPT
    ja .notfound
    movzx ebx, al
    movzx eax, byte [bp + 24]
    shr ax, 6
    and ax, 3
    shl ax, 8
    movzx edx, byte [bp + 25]
    or eax, edx
    movzx edx, byte [bp + 21]
    cmp dx, DISK_HEADS
    jae .notfound
    push ebx
    push edx
    mov ebx, DISK_HEADS
    mul ebx
    pop edx
    add eax, edx
    mov ebx, DISK_SPT
    mul ebx
    pop edx
    dec edx
    add eax, edx
    movzx edx, word [bp + 16]
    mov bx, es
    movzx ebx, bx
    shl ebx, 4
    add edx, ebx
    mov ebx, esi
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .invalid
    cmp ebx, LOAD_LIMIT
    ja .invalid
    push bp
    mov ebp, edx
    mov ecx, esi
    call disk_clear_stale
    call ata_write
    pop bp
    jc .ioerr_chs
    mov byte [bp + 29], ST_OK
    and word [bp + 36], 0xFFFE
    jmp .out

.ioerr_chs:
    mov byte [bp + 28], 0           ; no sector transferred
.ioerr:
    call disk_status
    mov [bp + 29], al
    or word [bp + 36], 1
    jmp .out

.notfound:
    mov byte [bp + 28], 0
    mov byte [bp + 29], ST_NOTFOUND
    or word [bp + 36], 1
    jmp .out

.invalid:
    mov byte [bp + 29], ST_BAD
    or word [bp + 36], 1
.out:
    pop es
    pop ds
    popad
    iret

; IN: SI = ata_read failure reason. OUT: AL = INT 13h status. Preserves SI.
disk_status:
    mov al, ST_BAD
    cmp si, msg_readerr
    jne .timeout
    mov al, ST_NOTFOUND
    ret
.timeout:
    cmp si, msg_timeout
    jne .nodrive
    mov al, ST_TIMEOUT
    ret
.nodrive:
    cmp si, msg_nodrive
    je .notready
    cmp si, msg_nodrive_slave
    jne .done
.notready:
    mov al, ST_NOTREADY
.done:
    ret

; Map DL (at [bp+20]) to ATA_DEVSEL. CF set if not 0x80 or 0x81.
disk_set_dev:
    mov al, [bp + 20]
    cmp al, 0x80
    je .master
    cmp al, 0x81
    je .slave
    stc
    ret
.master:
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    clc
    ret
.slave:
    mov byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    clc
    ret

; Parse a 16-byte disk address packet at DS:SI.
; Out: CF clear, EAX = LBA, CL = count, EDX = linear buffer; CF set if bad.
disk_parse_packet:
    call disk_set_dev
    jc .bad
    cmp byte [si], 0x10
    jne .bad
    movzx ecx, word [si + 2]
    test cx, cx
    jz .bad
    cmp cx, 128
    ja .bad
    cmp dword [si + 12], 0
    jne .bad
    mov eax, [si + 8]
    test eax, 0xF0000000
    jnz .bad
    movzx edx, word [si + 6]
    shl edx, 4
    movzx ebx, word [si + 4]
    add edx, ebx
    mov ebx, ecx
    shl ebx, 9
    add ebx, edx
    cmp edx, LOAD_LIMIT
    jae .bad
    cmp ebx, LOAD_LIMIT
    ja .bad
    clc
    ret
.bad:
    stc
    ret

; A failed command leaves ERR set, and ata_read refuses to start another
; while that bit is set. Drop it before a guest read. Preserves registers.
disk_clear_stale:
    push ax
    push dx
    mov dx, ATA_STATUS
    in al, dx
    test al, ST_ERR | ST_DF
    jz .ok
    call ata_reset
.ok:
    pop dx
    pop ax
    ret

; Stacked real-mode frame: [bp+2] = IP, [bp+4] = CS, [bp+6] = FLAGS.
; EAX=E820 memory map; AX=2400/2401/2402 A20 disable/enable/query via
; port 0x92 (same gate as enable_a20). AX=2403 reports port-0x92 support.
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

; Read with up to ATA_ATTEMPTS attempts, soft reset between them.
; In/Out: as ata_read. Does not touch EAX, ECX or EBP, so a retry
; repeats the same read. The attempt count lives in the BIOS data area
; because EBP is the caller's buffer address.
ata_read_retry:
    mov byte [ss:ATA_RETRY_CNT], ATA_ATTEMPTS
.try:
    call ata_read
    jnc .done
    cmp si, msg_nodrive         ; nothing to retry against
    je .fail
    cmp si, msg_nodrive_slave
    je .fail
    cmp si, msg_atapi
    je .fail
    cmp si, msg_atapi_slave
    je .fail
    dec byte [ss:ATA_RETRY_CNT]
    jz .fail
    call ata_reset
    jmp .try
.fail:
    stc
.done:
    ret

; Read sectors of the selected primary drive into memory below 1 MB.
; ATA_DEVSEL is master (0xE0) or slave (0xF0). In/Out as before.
; In:  EAX = LBA (bits 27:0; bits 31:28 must be 0),
;      CL  = sector count (1-128),
;      EBP = linear address of the destination.
; Out: CF = 0 on success. Every register preserved.
;      CF = 1 on failure, SI = reason, BL = status, BH = error register.
;      EAX, ECX, EDX, EBP, EDI and ES are preserved either way.
ata_read:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push es
    movzx esi, cl               ; sectors left
    mov edi, eax                ; LBA

    test si, si
    jz .badarg
    cmp si, 128
    ja .badarg
    test edi, 0xF0000000
    jnz .badarg

    ; 0xFF is a floating bus: no controller on the primary channel
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive_bus

    mov dx, ATA_DRIVE
    mov eax, edi
    shr eax, 24
    and al, 0x0F
    mov ah, [ss:ATA_DEVSEL]
    or al, ah                   ; master or slave, LBA
    out dx, al
    call ata_delay400

    ; 0x00 or 0xFF after selecting: nothing answered on that unit,
    ; unless an ATAPI device left its signature in the LBA registers.
    ; mid/hi both 0xFF with a ready status is QEMU's empty-unit quirk
    ; when the other device on the cable is the only one present.
    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive
    test al, al
    jz .check_sig
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0xFF
    jne .present
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xFF
    je .nodrive
    jmp .present
.check_sig:
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0x14
    jne .nodrive
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xEB
    je .atapi
    jmp .nodrive
.present:

    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov dx, ATA_COUNT
    mov ax, si
    out dx, al
    mov eax, edi
    mov dx, ATA_LBA_LO
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_MID
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_HI
    out dx, al

    mov dx, ATA_STATUS
    mov al, CMD_READ_SECTORS
    out dx, al

    cld
    ; One DRQ block per sector
.sector:
    call ata_delay400
    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov ecx, ebp                ; ES:DI = EBP, normalised so 512 bytes fit
    shr ecx, 4
    mov es, cx
    mov di, bp
    and di, 0x000F
    mov cx, 256
    mov dx, ATA_DATA
    rep insw
    add ebp, 512
    dec si
    jnz .sector

    ; Command must finish cleanly, otherwise the data is suspect
    call ata_delay400
    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error
    test al, ST_DRQ             ; drive still has data: count mismatch
    jnz .error
    clc
    jmp .ok

.badarg:
    mov si, msg_readerr
    xor bx, bx
    jmp .fail
.nodrive_bus:
    mov si, msg_nodrive         ; floating bus: always the master wording
    jmp .failed
.nodrive:
    mov si, msg_nodrive
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_nodrive_slave
    jmp .failed
.atapi:
    mov si, msg_atapi
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_atapi_slave
    jmp .failed
.timeout:
    mov si, msg_timeout
    jmp .failed
.error:
    mov si, msg_readerr
.failed:
    mov dx, ATA_STATUS
    in al, dx
    mov bl, al
    mov dx, ATA_ERROR
    in al, dx
    mov bh, al
.fail:
    pop es
    add sp, 8                   ; drop saved EBX and ESI; BL/BH/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc                         ; ADD above clears CF, so set it last
    ret
.ok:
    pop es
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret

; Write sectors of the selected primary drive from memory below 1 MB.
; Same register convention as ata_read.
ata_write:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push es
    movzx esi, cl
    mov edi, eax

    test si, si
    jz .badarg
    cmp si, 128
    ja .badarg
    test edi, 0xF0000000
    jnz .badarg

    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive_bus

    mov dx, ATA_DRIVE
    mov eax, edi
    shr eax, 24
    and al, 0x0F
    mov ah, [ss:ATA_DEVSEL]
    or al, ah
    out dx, al
    call ata_delay400

    mov dx, ATA_STATUS
    in al, dx
    cmp al, 0xFF
    je .nodrive
    test al, al
    jz .check_sig
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0xFF
    jne .present
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xFF
    je .nodrive
    jmp .present
.check_sig:
    mov dx, ATA_LBA_MID
    in al, dx
    cmp al, 0x14
    jne .nodrive
    mov dx, ATA_LBA_HI
    in al, dx
    cmp al, 0xEB
    je .atapi
    jmp .nodrive
.present:

    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov dx, ATA_COUNT
    mov ax, si
    out dx, al
    mov eax, edi
    mov dx, ATA_LBA_LO
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_MID
    out dx, al
    shr eax, 8
    mov dx, ATA_LBA_HI
    out dx, al

    mov dx, ATA_STATUS
    mov al, CMD_WRITE_SECTORS
    out dx, al

    cld
.sector:
    call ata_delay400
    mov bl, ST_DRQ | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error

    mov ecx, ebp
    shr ecx, 4
    mov es, cx
    mov di, bp
    and di, 0x000F
    push ds
    push si
    mov si, di
    mov ax, es
    mov ds, ax
    mov cx, 256
    mov dx, ATA_DATA
    rep outsw
    pop si
    pop ds
    add ebp, 512
    dec si
    jnz .sector

    call ata_delay400
    mov bl, ST_DRDY | ST_ERR | ST_DF
    call ata_poll
    jc .timeout
    test al, ST_ERR | ST_DF
    jnz .error
    test al, ST_DRQ
    jnz .error
    clc
    jmp .ok

.badarg:
    mov si, msg_readerr
    xor bx, bx
    jmp .fail
.nodrive_bus:
    mov si, msg_nodrive
    jmp .failed
.nodrive:
    mov si, msg_nodrive
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_nodrive_slave
    jmp .failed
.atapi:
    mov si, msg_atapi
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    jne .failed
    mov si, msg_atapi_slave
    jmp .failed
.timeout:
    mov si, msg_timeout
    jmp .failed
.error:
    mov si, msg_readerr
.failed:
    mov dx, ATA_STATUS
    in al, dx
    mov bl, al
    mov dx, ATA_ERROR
    in al, dx
    mov bh, al
.fail:
    pop es
    add sp, 8
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret
.ok:
    pop es
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret

; Poll status until BSY is clear and any bit in BL is set.
; In: BL = mask.  Out: AL = status, CF = 1 on timeout.
ata_poll:
    mov ecx, ATA_TIMEOUT
    mov dx, ATA_STATUS
.poll:
    in al, dx
    test al, ST_BSY
    jnz .next
    test al, bl
    jnz .done
.next:
    dec ecx
    jnz .poll
    stc
    ret
.done:
    clc
    ret

; Software reset of the primary channel, then wait for BSY to clear.
; Preserves every register but the flags.
ata_reset:
    push eax
    push ebx
    push ecx
    push edx
    mov dx, ATA_CTRL
    mov al, CTRL_SRST | CTRL_NIEN
    out dx, al
    call ata_delay400           ; SRST must be held >= 5 us
    call ata_delay400
    call ata_delay400
    mov al, CTRL_NIEN
    out dx, al
    mov bl, 0xFF & ~ST_BSY      ; any status once BSY drops
    call ata_poll
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ~400 ns: four reads of the alternate status register.
ata_delay400:
    push ax
    push dx
    mov dx, ATA_CTRL
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop dx
    pop ax
    ret

; ---------------------------------------------------------------------------
; Console: VGA text buffer and COM1 together
; ---------------------------------------------------------------------------

; In: DS:SI = zero-terminated string, ES:DI = VGA cell, AH = attribute.
; Out: DI advanced past the string.
con_print:
    lodsb
    test al, al
    jz .done
    call con_putc
    jmp con_print
.done:
    ret

; In: AL = byte, ES:DI = VGA cell, AH = attribute.  Prints two hex digits.
con_hex8:
    push cx
    mov cl, al
    shr al, 4
    call .nibble
    mov al, cl
    and al, 0x0F
    call .nibble
    pop cx
    ret
.nibble:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 'A' - '9' - 1
.out:
    jmp con_putc

; Move DI to the start of the next VGA row, CR LF on COM1.
con_newline:
    push ax
    push cx
    push dx
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    mov ax, di
    xor dx, dx
    mov cx, VGA_LINE
    div cx
    inc ax
    mul cx
    mov di, ax
    pop dx
    pop cx
    pop ax
    ret

; In: AL = character, AH = attribute, ES:DI = VGA cell.  Out: DI += 2.
con_putc:
    stosw
    jmp serial_putc

; COM1: 115200 baud, 8N1, FIFOs on, interrupts off.
serial_init:
    mov dx, COM1 + 1            ; IER
    xor al, al
    out dx, al
    mov dx, COM1 + 3            ; LCR: DLAB on
    mov al, 0x80
    out dx, al
    mov dx, COM1                ; divisor 1 = 115200
    mov al, 1
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3            ; LCR: 8N1, DLAB off
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2            ; FCR: enable and clear FIFOs
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4            ; MCR: DTR, RTS
    mov al, 0x03
    out dx, al
    ret

; In: AL = character.  Gives up after UART_TIMEOUT polls so a missing
; or wedged UART cannot hang the boot.
serial_putc:
    push ax
    push cx
    push dx
    mov ah, al
    mov cx, UART_TIMEOUT
    mov dx, COM1 + 5            ; LSR
.wait:
    in al, dx
    test al, 0x20               ; THR empty
    jnz .send
    loop .wait
.send:
    mov al, ah
    mov dx, COM1
    out dx, al
    pop dx
    pop cx
    pop ax
    ret

%include "memmap.inc"
%include "vga.inc"

banner      db 'SquirrelBIOS x86', 0
msg_nodrive db 'disk: no drive on primary master', 0
msg_nodrive_slave db 'disk: no drive on primary slave', 0
msg_atapi   db 'disk: primary master is ATAPI (CD/DVD), not supported', 0
msg_atapi_slave db 'disk: primary slave is ATAPI (CD/DVD), not supported', 0
msg_timeout db 'disk: timeout waiting for drive', 0
msg_readerr db 'disk: read of LBA 0 failed', 0
msg_nosig   db 'disk: boot sector has no 0xAA55 signature', 0
msg_readerr_extra db 'disk: read of the extra sectors failed', 0
msg_kernel  db 'disk: read of the kernel image failed', 0
msg_nosig_os db 'disk: kernel image has no S2OK signature', 0
msg_kload   db 'kernel loaded at 0x00008000, jumping', 0
msg_bzload  db 'bzImage loaded at 0x00100000, jumping', 0
msg_bzbad   db 'disk: bzImage header is invalid', 0
msg_bzread  db 'disk: read of the bzImage failed', 0
msg_overlap db 'disk: load overlaps firmware data', 0
msg_range   db 'disk: load address is out of range', 0
msg_status  db '  status=', 0
msg_error   db ' error=', 0
msg_halted  db 'boot halted', 0

; SMBIOS then ACPI at fixed ROM offsets, then pad to the reset vector.
%include "smbios.inc"
%include "acpi.inc"

times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

times 65536 - ($ - $$) db 0
