; SquirrelBIOS — minimal ACPI 1.0 tables in the ROM
; Original code.
;
; Placed at fixed offsets so physical addresses and checksums are stable.
; The ROM is mapped at 0xF0000, so offset O is physical 0xF0000+O.
;
;   0xFE000  RSDP  ("RSD PTR "), revision 0, 20 bytes
;   0xFE020  RSDT  (one entry: FADT), 40 bytes
;   0xFE050  FADT  ("FACP"), length 116, DSDT pointer, no FACS
;   0xFE0D0  DSDT  header only (no AML objects yet), 36 bytes
;
; Scan 0xE0000–0xFFFFF for the RSDP. acpi_init is a no-op at the call
; site; the tables are static in the ROM image.

ACPI_RSDP_OFF   equ 0xE000
ACPI_RSDT_OFF   equ 0xE020
ACPI_FADT_OFF   equ 0xE050
ACPI_DSDT_OFF   equ 0xE0D0
ACPI_ROM_BASE   equ 0xF0000
ACPI_RSDT_PHYS  equ ACPI_ROM_BASE + ACPI_RSDT_OFF
ACPI_FADT_PHYS  equ ACPI_ROM_BASE + ACPI_FADT_OFF
ACPI_DSDT_PHYS  equ ACPI_ROM_BASE + ACPI_DSDT_OFF

; Table checksums (sum of every byte = 0). x86-acpi checks them, so a
; field change that forgets to update these fails make test.
ACPI_FADT_SUM   equ 0x6F
ACPI_DSDT_SUM   equ 0x69

acpi_init:
    ret

times ACPI_RSDP_OFF - ($ - $$) db 0

; Checksum computed for rsdt = 0xFE020.
acpi_rsdp:
    db 'RSD PTR '
    db 0x1E
    db 'SQRRL '
    db 0
    dd ACPI_RSDT_PHYS
acpi_rsdp_end:
%if (acpi_rsdp_end - acpi_rsdp) != 20
%error "RSDP must be 20 bytes"
%endif

times ACPI_RSDT_OFF - ($ - $$) db 0

; Checksum for fadt = 0xFE050.
acpi_rsdt:
    db 'RSDT'
    dd 40
    db 1
    db 0x18
    db 'SQRRL '
    db 'SQUIRREL'
    dd 1
    db 'SQR '
    dd 1
    dd ACPI_FADT_PHYS
acpi_rsdt_end:
%if (acpi_rsdt_end - acpi_rsdt) != 40
%error "RSDT must be 40 bytes"
%endif

times ACPI_FADT_OFF - ($ - $$) db 0

; Checksum for dsdt = 0xFE0D0. Linux loads the namespace from the DSDT
; the FADT names, and faults if there is none.
acpi_fadt:
    db 'FACP'
    dd 116
    db 1
    db ACPI_FADT_SUM
    db 'SQRRL '
    db 'SQUIRREL'
    dd 1
    db 'SQR '
    dd 1
    dd 0                        ; 36: FIRMWARE_CTRL (FACS), none
    dd ACPI_DSDT_PHYS           ; 40: DSDT
    times 116 - ($ - acpi_fadt) db 0
acpi_fadt_end:
%if (acpi_fadt_end - acpi_fadt) != 116
%error "FADT must be 116 bytes"
%endif

times ACPI_DSDT_OFF - ($ - $$) db 0

; A definition block with no objects: valid AML, nothing to evaluate.
acpi_dsdt:
    db 'DSDT'
    dd 36
    db 1
    db ACPI_DSDT_SUM
    db 'SQRRL '
    db 'SQUIRREL'
    dd 1
    db 'SQR '
    dd 1
acpi_dsdt_end:
%if (acpi_dsdt_end - acpi_dsdt) != 36
%error "DSDT must be 36 bytes"
%endif
