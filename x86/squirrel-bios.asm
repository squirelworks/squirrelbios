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
;     sectors it asks for (LBA 1 on) to its load address, below the EBDA
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
; Build configuration (configs/x86-pc/*.cfg, see configs/README.md).
; make PLATFORM=x86-pc passes -DSQ_CONFIG and the generated config.inc;
; plain make uses the defaults below, which are what default.cfg says.
%ifdef SQ_CONFIG
%include "config.inc"
%endif
%ifndef CONFIG_COM1_BASE
%define CONFIG_COM1_BASE 0x3F8
%endif
%ifndef CONFIG_ATA_ATTEMPTS
%define CONFIG_ATA_ATTEMPTS 3
%endif
; Modules (1 = built in). configs/x86-pc/minimal.cfg turns them off.
%ifndef CONFIG_ACPI
%define CONFIG_ACPI 1
%endif
%ifndef CONFIG_SMBIOS
%define CONFIG_SMBIOS 1
%endif
%ifndef CONFIG_BZIMAGE
%define CONFIG_BZIMAGE 1
%endif
%ifndef CONFIG_OPTROM
%define CONFIG_OPTROM 1
%endif
%ifndef CONFIG_FLOPPY
%define CONFIG_FLOPPY 1
%endif
%ifndef CONFIG_PCI
%define CONFIG_PCI 1
%endif
%ifndef CONFIG_CDBOOT
%define CONFIG_CDBOOT 1
%endif
%ifndef CONFIG_USB
%define CONFIG_USB CONFIG_PCI
%endif
%if CONFIG_USB && !CONFIG_PCI
%error "CONFIG_USB needs CONFIG_PCI"
%endif
%ifndef CONFIG_NVME
%define CONFIG_NVME CONFIG_PCI
%endif
%if CONFIG_NVME && !CONFIG_PCI
%error "CONFIG_NVME needs CONFIG_PCI"
%endif
%ifndef CONFIG_AHCI
%define CONFIG_AHCI CONFIG_PCI
%endif
%if CONFIG_AHCI && !CONFIG_PCI
%error "CONFIG_AHCI needs CONFIG_PCI"
%endif

FW_STACK        equ 0x0600

; SQBL header at the end of sector 0 (x86-extra-sectors)
BOOT_SECTOR     equ 0x7C00
SQBL_HDR        equ BOOT_SECTOR + 0x1F0
SQBL_MAGIC      equ 0x4C425153  ; bytes 53 51 42 4C, 'SQBL'
SQBL_MAX        equ 128         ; extra sectors
LOAD_LIMIT      equ EBDA_BASE   ; first byte a load or a buffer may not reach

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
BZ_BOUNCE       equ 0x8000      ; BZ_CHUNK sectors, ends at 0x10000
BZ_CHUNK        equ 64
BZ_BOOT_PARAMS  equ 0x90000     ; boot_params, 4 KB
BZ_CMDLINE      equ 0x91000     ; kernel command line (cmd_line_ptr)
BZ_STACK        equ 0x90000     ; grows down, below boot_params
BZ_HDR_MAX      equ 0x290       ; setup header ends before e820_table
BZ_E820_MAX     equ 128
BZ_LOADER_TYPE  equ 0xFF        ; boot.rst: undefined boot loader
BZ_BOOT_CS      equ 0x10        ; __BOOT_CS
BZ_BOOT_DS      equ 0x18        ; __BOOT_DS

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
CMD_IDENTIFY      equ 0xEC
CMD_READ_EXT      equ 0x24      ; READ SECTORS EXT (LBA48)
CMD_WRITE_EXT     equ 0x34      ; WRITE SECTORS EXT

ATA_TIMEOUT     equ 0x00400000  ; status polls before giving up
ATA_ATTEMPTS    equ CONFIG_ATA_ATTEMPTS
; Drive select base (LBA bit set): master 0xE0, slave 0xF0. Boot DL is
; 0x80 or 0x81. Both live in the page below the memory map; 0x04FE is
; the ATA retry counter and 0x04FB which units answered IDENTIFY at POST
; (bit 0 master, bit 1 slave).
ATA_PRESENT     equ 0x04FB
ATA_DEVSEL      equ 0x04FC
BOOT_DL         equ 0x04FD
ATA_RETRY_CNT   equ 0x04FE
DISK_KIND       equ 0x04FF      ; what DL 80h is: 0 IDE, else one of these
DISK_AHCI       equ 1
DISK_NVME       equ 2
ATA_SEL_MASTER  equ 0xE0
ATA_SEL_SLAVE   equ 0xF0

COM1            equ CONFIG_COM1_BASE
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
BDA_HDCOUNT     equ 0x0475      ; byte: number of hard disks
BDA_TICKS       equ 0x046C      ; dword: PIT ticks since midnight (IRQ0)
BDA_TICK_OVR    equ 0x0470      ; byte: 24-hour overflow flag
BDA_KBD_HEAD    equ 0x041A      ; keyboard buffer head (offset into segment 0)
BDA_KBD_TAIL    equ 0x041C      ; keyboard buffer tail
BDA_KBD_BUF     equ 0x041E      ; 16 words: ASCII, then scancode
BDA_KBD_END     equ 0x043E      ; first byte past the buffer
; Equipment: 80x25 colour (bits 4-5 = 10), one serial port (bits 9-11 = 1).
; No floppies, no parallel ports. Matches COM1 + VGA text in this firmware.
BDA_EQUIP_VAL   equ 0x0220
BDA_MEM_KB      equ EBDA_BASE >> 10 ; 0x00000-0x95FFF, as in the memory map
BDA_EBDA_SEG    equ 0x040E      ; segment of the extended BIOS data area
EBDA_BASE       equ 0x96000     ; 40 KB EBDA just below the VGA window:
EBDA_SIZE       equ 0xA000      ; a page of firmware data, then 4 KB-aligned
                                ; pages for controller queues (NVMe, +1000h
                                ; to +4FFFh) and for the USB controller that
                                ; keeps running after POST (+5000h on)
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
ST_PARAMS       equ 0x07        ; drive parameter activity failed
ST_TIMEOUT      equ 0x80
ST_NOTREADY     equ 0xAA
EDD_VERSION     equ 0x30        ; INT 13h AH=41h: EDD 3.0
EDD_SUBSET_FIXED equ 0x0001     ; 42h, 43h, 44h, 47h, 48h

VGA_SEG         equ 0xB800
VGA_LINE        equ 160         ; bytes per 80-column text row
ATTR_NORMAL     equ 0x0F
ATTR_ERROR      equ 0x0C

%include "core/entry.inc"

%if CONFIG_BZIMAGE
%include "boot/bzimage.inc"
%endif

%include "boot/handoff.inc"

%if CONFIG_OPTROM
%include "boot/optrom.inc"
%endif

%include "boot/fail.inc"

%include "core/post.inc"

%include "drivers/kbd8042/kbd.inc"

%include "core/ivt.inc"

%include "drivers/kbd8042/int16.inc"

%include "drivers/rtc/int1a.inc"

%include "drivers/vga/int10.inc"

%include "drivers/ata/int13.inc"

%include "core/int15.inc"

%include "drivers/ata/ata.inc"

%if CONFIG_FLOPPY
%include "drivers/fdc/fdc.inc"
%include "drivers/fdc/int13fd.inc"
%include "boot/floppy.inc"
%endif

%if CONFIG_CDBOOT
%include "drivers/atapi/atapi.inc"
%include "drivers/atapi/int13cd.inc"
%include "boot/eltorito.inc"
%endif

%if CONFIG_AHCI
%include "drivers/ahci/ahci.inc"
%endif
%if CONFIG_NVME
%include "drivers/nvme/nvme.inc"
%endif
%if CONFIG_USB
%include "drivers/usb/usb.inc"
%include "drivers/usb/uhci.inc"
%include "drivers/usb/ohci.inc"
%include "drivers/usb/ehci.inc"
%include "drivers/usb/xhci.inc"
%endif

%if CONFIG_PCI
%include "drivers/pci/pci.inc"
%include "drivers/pci/int1apci.inc"
%include "drivers/pci/pir.inc"
%endif

%include "core/console.inc"

%include "core/memmap.inc"
%include "drivers/vga/vga.inc"

banner      db 'SquirrelBIOS x86', 0
msg_nodrive db 'disk: no drive on primary master', 0
msg_nodrive_slave db 'disk: no drive on primary slave', 0
msg_atapi   db 'disk: primary master is ATAPI (CD/DVD), not a hard disk', 0
msg_atapi_slave db 'disk: primary slave is ATAPI (CD/DVD), not a hard disk', 0
msg_timeout db 'disk: timeout waiting for drive', 0
msg_readerr db 'disk: read of LBA 0 failed', 0
msg_nosig   db 'disk: boot sector has no 0xAA55 signature', 0
msg_readerr_extra db 'disk: read of the extra sectors failed', 0
msg_kernel  db 'disk: read of the kernel image failed', 0
msg_nosig_os db 'disk: kernel image has no S2OK signature', 0
msg_kload   db 'kernel loaded at 0x00008000, jumping', 0
%if CONFIG_FLOPPY
msg_fdboot  db 'booting from floppy A:', 0
msg_fdnosig db 'floppy: boot sector has no 0xAA55 signature, trying the hard disk', 0
%endif
%if CONFIG_BZIMAGE
msg_bzload  db 'bzImage loaded at 0x00100000, jumping', 0
msg_bzbad   db 'disk: bzImage header is invalid', 0
msg_bzread  db 'disk: read of the bzImage failed', 0
msg_bzbig   db 'disk: bzImage does not fit in RAM', 0
bz_cmdline  db 'console=ttyS0,115200', 0
%endif
%if CONFIG_AHCI
msg_ahci    db 'disk: SATA disk on AHCI port ', 0
%endif
%if CONFIG_USB
msg_usb     db 'usb: ', 0
msg_usb_uhci db 'UHCI at ', 0
msg_usb_ohci db 'OHCI at ', 0
msg_usb_ehci db 'EHCI at ', 0
msg_usb_xhci db 'xHCI at ', 0
msg_usb_dev db 'device ', 0
msg_usb_on_root db ' on root port ', 0
msg_usb_on_hub db ' on hub ', 0
msg_usb_port db ' port ', 0
msg_usb_colon db ': ', 0
msg_usb_comma db ', ', 0
msg_usb_hid db 'HID', 0
msg_usb_msd db 'mass storage', 0
msg_usb_hub db 'hub', 0
msg_usb_class db 'class ', 0
msg_usb_ports db ' ports', 0
msg_usb_fail db 'no answer from the device on port ', 0
%endif
%if CONFIG_NVME
msg_nvme    db 'disk: NVMe disk (namespace 1)', 0
%endif
%if CONFIG_CDBOOT
msg_cdboot  db 'booting from CD-ROM (El Torito, no emulation)', 0
msg_cdread  db 'cd: read of the CD failed', 0
msg_cdnoboot db 'cd: no El Torito boot record', 0
msg_cdcatalog db 'cd: boot catalog is invalid', 0
msg_cdnotboot db 'cd: default entry is not bootable', 0
msg_cdemul  db 'cd: only no-emulation boot images are supported', 0
msg_cdrange db 'cd: boot image load address is out of range', 0
%endif
msg_overlap db 'disk: load overlaps firmware data', 0
msg_range   db 'disk: load address is out of range', 0
msg_status  db '  status=', 0
msg_error   db ' error=', 0
msg_halted  db 'boot halted', 0

; SMBIOS then ACPI at fixed ROM offsets, then pad to the reset vector.
%if CONFIG_SMBIOS
%include "tables/smbios/smbios.inc"
%endif
%if CONFIG_ACPI
%include "tables/acpi/acpi.inc"
%endif

times (65536 - 16) - ($ - $$) db 0

reset_vector:
    jmp 0xF000:entry

times 65536 - ($ - $$) db 0
