; SquirrelBIOS x86 — $PIR PCI interrupt routing table
; Original code. Included by x86/squirrel-bios.asm when CONFIG_PCI.
;
; Layout from the PCI IRQ Routing Table Specification 1.0: a 32-byte
; header on a 16-byte boundary in 0xF0000-0xFFFFF, then one 16-byte entry
; per device. It describes QEMU's i440fx: the PIIX3 at 00:01.0 routes
; PIRQA-D (link values 0x60-0x63, its route registers), and device d's
; INTx (x = 0 for INTA) reaches PIRQ (d - 1 + x) mod 4. Devices 1-31 are
; listed as embedded (slot 0). On other chipsets the router it names is
; not there and an OS ignores the table.

PIR_DEVS        equ 31              ; devices 1-31 on bus 0
PIR_SIZE        equ 32 + 16 * PIR_DEVS
PIR_IRQS        equ 0xDEF8          ; IRQ 3-7, 9-12, 14, 15

; Byte sum of everything but the checksum, worked out at assembly time
%assign pir_sum '$' + 'P' + 'I' + 'R' + 0x01 + (PIR_SIZE & 0xFF) + (PIR_SIZE >> 8)
%assign pir_sum pir_sum + (PIIX3_BDF & 0xFF) + 0x86 + 0x80 + 0x00 + 0x70
%assign pir_dev 1
%rep PIR_DEVS
%assign pir_sum pir_sum + (pir_dev << 3)
%assign pir_pin 0
%rep 4
%assign pir_sum pir_sum + PIIX3_PIRQ + ((pir_dev - 1 + pir_pin) & 3) + (PIR_IRQS & 0xFF) + (PIR_IRQS >> 8)
%assign pir_pin pir_pin + 1
%endrep
%assign pir_dev pir_dev + 1
%endrep

align 16, db 0
pir_table:
    db '$PIR'
    dw 0x0100                       ; version 1.0
    dw PIR_SIZE
    db 0                            ; router bus
    db PIIX3_BDF & 0xFF             ; router device << 3 | function
    dw 0                            ; no IRQs kept for PCI alone
    dw 0x8086, 0x7000               ; compatible router: PIIX3
    dd 0                            ; miniport data
    times 11 db 0
    db (0x100 - (pir_sum & 0xFF)) & 0xFF
%assign pir_dev 1
%rep PIR_DEVS
    db 0, pir_dev << 3              ; bus, device
%assign pir_pin 0
%rep 4
    db PIIX3_PIRQ + ((pir_dev - 1 + pir_pin) & 3)
    dw PIR_IRQS
%assign pir_pin pir_pin + 1
%endrep
    db 0, 0                         ; slot (embedded), reserved
%assign pir_dev pir_dev + 1
%endrep
pir_end:
%if pir_end - pir_table != PIR_SIZE
%error "$PIR table size"
%endif
