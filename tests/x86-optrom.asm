; Minimal legacy option ROM for the firmware's scan.
; Original code.
;
; Signature 55 AA, size 1 (512 bytes), init at offset 3. Prints
; optrom=ok on COM1 and returns with RETF. Checksum of the 512 bytes is 0.

BITS 16
ORG 0

    db 0x55, 0xAA
    db 1                        ; size in 512-byte units
    jmp short init              ; offset 3 entry

init:
    push ax
    push dx
    push ds
    push si
    push cs
    pop ds
    mov si, msg
.putc:
    lodsb
    test al, al
    jz .done
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    jmp .putc
.done:
    pop si
    pop ds
    pop dx
    pop ax
    retf

msg db 'optrom=ok', 13, 10, 0

times 511 - ($ - $$) db 0
; Last byte chosen so the 512-byte sum is 0 (filled by the build rule).
cksum db 0
