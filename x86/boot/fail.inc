; SquirrelBIOS x86 — failure reports and halt
; Original code. Included by x86/squirrel-bios.asm.

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
