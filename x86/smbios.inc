; SquirrelBIOS — minimal SMBIOS 2.4 entry point and structures
; Original code.
;
; Fixed ROM offsets (ROM at 0xF0000):
;   0xFD000  Entry point (_SM_ / _DMI_), 31 bytes
;   0xFD020  Structure table (Type 0, Type 1, Type 127)
;
; Scan 0xF0000–0xFFFFF on a 16-byte stride for "_SM_". Include this
; file before acpi.inc so the 0xD000 pad runs first.

SMBIOS_EPS_OFF  equ 0xD000
SMBIOS_TBL_OFF  equ 0xD020
SMBIOS_ROM_BASE equ 0xF0000
SMBIOS_TBL_PHYS equ SMBIOS_ROM_BASE + SMBIOS_TBL_OFF

smbios_init:
    ret

times SMBIOS_EPS_OFF - ($ - $$) db 0

; Checksums: EPS byte sum 0; intermediate (_DMI_..BCD) sum 0.
; Max structure size 66; table length 127; three structures.
smbios_eps:
    db '_SM_'
    db 0x3B                     ; EPS checksum
    db 0x1F
    db 2, 4
    dw 66                       ; max structure size
    db 0
    times 5 db 0
    db '_DMI_'
    db 0xC3                     ; intermediate checksum
    dw 127                      ; structure table length
    dd SMBIOS_TBL_PHYS
    dw 3
    db 0x24
smbios_eps_end:
%if (smbios_eps_end - smbios_eps) != 0x1F
%error "SMBIOS EPS must be 31 bytes"
%endif

times SMBIOS_TBL_OFF - ($ - $$) db 0

smbios_tbl:
smbios_type0:
    db 0
    db 24
    dw 0x0000
    db 1                        ; vendor
    db 2                        ; version
    dw 0xE800
    db 3                        ; date
    db 0                        ; ROM size code (64 KB)
    dq 0
    db 0, 0
    db 1, 0
    db 0xFF, 0xFF
    db 'SquirrelBIOS', 0
    db '1.0.0', 0
    db '11/01/2026', 0
    db 0

smbios_type1:
    db 1
    db 27
    dw 0x0001
    db 1, 2, 3, 4
    times 16 db 0
    db 6, 0, 0
    db 'Squirrel Works', 0
    db 'SquirrelBIOS PC', 0
    db '1', 0
    db 'None', 0
    db 0

smbios_type127:
    db 127
    db 4
    dw 0x0002
    db 0, 0
smbios_tbl_end:
%if (smbios_tbl_end - smbios_tbl) != 127
%error "SMBIOS table length must be 127"
%endif
