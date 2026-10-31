; SquirrelBIOS — x86 firmware ROM for QEMU PC
; 64 KB image. Reset vector at offset 0xFFF0.
; Original code.
;
; Build:  nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
; Run:    qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide
;
; Behaviour:
;   - Set segments and stack
;   - Build a memory map at 0x0000:0x0500 (memmap.inc)
;   - Print banner to VGA text
;   - ATA PIO read of LBA 0 (sector 1) into 0x7C00
;   - Jump to 0x0000:0x7C00 with DL = 0x80, DI = 0x0500 (memory map)

BITS 16

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    call build_memmap

    ; Banner
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov si, banner
    mov ah, 0x0F
.print:
    lodsb
    test al, al
    jz .disk
    stosw
    jmp .print

.disk:
    ; ATA PIO read — primary master, LBA 0, 1 sector → 0x7C00
    mov dx, 0x1F7
.wait1:
    in al, dx
    test al, 0x80          ; BSY
    jnz .wait1
    test al, 0x40          ; DRDY
    jz .wait1

    mov dx, 0x1F6
    mov al, 0xE0          ; master, LBA
    out dx, al

    mov dx, 0x1F2
    mov al, 1             ; sector count
    out dx, al

    mov dx, 0x1F3
    xor al, al            ; LBA 0
    out dx, al
    mov dx, 0x1F4
    out dx, al
    mov dx, 0x1F5
    out dx, al

    mov dx, 0x1F7
    mov al, 0x20          ; READ SECTORS
    out dx, al

.wait2:
    in al, dx
    test al, 0x80
    jnz .wait2
    test al, 0x08          ; DRQ
    jz .fail

    mov dx, 0x1F0
    mov di, 0x7C00
    mov cx, 256
    rep insw

    ; Hand-off
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov dl, 0x80          ; boot drive
    mov di, MEMMAP_ADDR   ; memory map header
    jmp 0x0000:0x7C00

.fail:
    mov si, failmsg
    mov di, 160           ; second line
    mov ah, 0x0C
.pfail:
    lodsb
    test al, al
    jz .halt
    stosw
    jmp .pfail

.halt:
    hlt
    jmp .halt

%include "memmap.inc"

banner  db 'SquirrelBIOS x86', 0
failmsg db 'disk fail', 0

; Pad so the reset vector lands at offset 0xFFF0
times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

times 65536 - ($ - $$) db 0
