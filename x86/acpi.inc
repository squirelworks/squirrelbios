; SquirrelBIOS — minimal ACPI 1.0 tables in the ROM
; Original code.
;
; Placed at fixed offsets so physical addresses and checksums are stable.
; The ROM is mapped at 0xF0000, so offset O is physical 0xF0000+O.
;
;   0xFE000  RSDP  ("RSD PTR "), revision 0, 20 bytes
;   0xFE020  RSDT  (one entry: FADT), 40 bytes
;   0xFE050  FADT  ("FACP"), length 116, no FACS/DSDT
;
; Scan 0xE0000–0xFFFFF for the RSDP. acpi_init is a no-op at the call
; site; the tables are static in the ROM image.

ACPI_RSDP_OFF   equ 0xE000
ACPI_RSDT_OFF   equ 0xE020
ACPI_FADT_OFF   equ 0xE050
ACPI_ROM_BASE   equ 0xF0000
ACPI_RSDT_PHYS  equ ACPI_ROM_BASE + ACPI_RSDT_OFF
ACPI_FADT_PHYS  equ ACPI_ROM_BASE + ACPI_FADT_OFF

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

acpi_fadt:
    db 'FACP'
    dd 116
    db 1
    db 0x2E
    db 'SQRRL '
    db 'SQUIRREL'
    dd 1
    db 'SQR '
    dd 1
    times 116 - ($ - acpi_fadt) db 0
acpi_fadt_end:
%if (acpi_fadt_end - acpi_fadt) != 116
%error "FADT must be 116 bytes"
%endif
