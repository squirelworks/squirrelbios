# x86 SquirrelBIOS

64 KB ROM for QEMU PC. `squirrel-bios.asm` holds the constants, the
build options and the ROM layout, and includes one file per module:

```
core/      entry (POST and first boot decision), post (BDA, A20, APIC,
           PIT/PIC), ivt (IVT, IRQ0), int15, console, memmap
boot/      handoff, fail, floppy (A: boot; CONFIG_FLOPPY),
           eltorito (CD boot; CONFIG_CDBOOT),
           bzimage (CONFIG_BZIMAGE), optrom (CONFIG_OPTROM)
drivers/   ata (ATA PIO, LBA28/LBA48, INT 13h with EDD 3.0),
           fdc (82077 FDC, INT 13h diskettes; CONFIG_FLOPPY),
           atapi (ATAPI on both IDE channels, INT 13h for the boot CD;
           CONFIG_CDBOOT), vga (INT 10h, text mode, font),
           kbd8042 (8042, IRQ1, INT 16h), rtc (INT 1Ah),
           pci (bus 0 scan, BARs, IRQ routing, INT 1Ah B1xx, $PIR;
           CONFIG_PCI), ahci (one SATA disk as DL 80h when no IDE disk
           answered, DMA through the EBDA; CONFIG_AHCI),
           nvme (namespace 1 as DL 80h when no IDE or SATA disk
           answered, queues in the EBDA; CONFIG_NVME),
           usb (USB core: enumeration and hubs; uhci, ohci;
           CONFIG_USB)
tables/    acpi (CONFIG_ACPI), smbios (CONFIG_SMBIOS)
```

The modules are included in a fixed order, so the default build is the
same ROM, byte for byte, as before the split. `configs/x86-pc/minimal.cfg`
leaves out the four optional modules (`make PLATFORM=x86-pc CONFIG=minimal`).

```bash
nasm -f bin squirrel-bios.asm -o squirrel-bios.rom   # run from x86/, or pass -i x86/
qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio
```

- Reset vector at offset 0xFFF0
- Programs COM1 at 115200 8N1
- Puts the VGA in 80x25 colour text mode and loads its own 8x16 font
  (`drivers/vga/vga.inc`, `font.inc`); every message goes to the screen and COM1
- Fills the BIOS data area equipment list (`0x0410` = `0x0220`),
  conventional memory KB (`0x0413` = 620, below a 20 KB EBDA at `0x9B000`
  that `0x040E` points at), and page-0 cursor (`0x0450`)
- Enables A20 through port `0x92` before the hand-off jump
- Puts the local APIC in virtual-wire mode (ExtINT) when present, then
  programs the PIT (~18.2 Hz) and both PICs (IRQ0 at `INT 08h`, IRQ1 at
  `INT 09h`); IRQ0 increments the BDA tick dword at `0x046C`
- Enables the i8042 with set-1 translation; `INT 16h` `AH=0x00`/`0x01`
  read the BDA keyboard buffer filled by IRQ1
- `INT 1Ah` `AH=0x00` returns the tick count; `AH=0x02` reads RTC time
  in BCD from CMOS
- `INT 10h` also sets/gets the cursor, scrolls, and reports mode 3
- `INT 13h` `AH=0x03` / `0x43` write the primary master or slave
  (`DL=0x80` / `0x81`, CHS / LBA)
- `INT 15h` `AX=0x2400`–`0x2403` control and query the A20 gate
- Places a SMBIOS 2.4 entry point at `0xFD000` (`tables/smbios/`) and a
  minimal ACPI 1.0 RSDP/RSDT/FADT/DSDT at `0xFE000` (`tables/acpi/`)
- Detects a Linux `bzImage` (`HdrS`), loads the whole PM kernel at 1 MB
  (any size that fits in RAM), fills boot_params (setup header, E820
  table, command line, `type_of_loader`) and enters it through the 32-bit
  boot protocol (`CS = 0x10`, `DS = 0x18`, `ESI` = boot_params). Ubuntu
  24.04's amd64 kernel starts on it (`make linux-x86`)
- Scans option ROMs at `0xD0000`–`0xEFFFF` (`55 AA` + checksum) and
  far-calls each init
- Prints banner
- ATA PIO reads LBA 0 into `0x7C00`, with timeouts on every status wait,
  `ERR`/`DF` checks, and up to 3 attempts with a soft reset in between;
  the master and slave are first probed with `IDENTIFY DEVICE`, and the
  boot read goes to the first one that answered
- Detects a missing drive or an ATAPI device and says so instead of hanging
- On failure prints the reason and the ATA status/error registers, then halts
- Refuses a sector whose last word is not `0xAA55` (not retried), then halts
- When the sector has an SQBL header at `0x1F0`, reads the 1–128 sectors
  after it to the header's `segment:offset` (below the EBDA, clear of the
  memory map and the boot sector) before jumping to `0x7C00`
- When it does not, and LBA 192 ends in `S2OK`, reads LBAs 1–192 to
  `0x8000` and jumps there. The boot sector is not entered
- Otherwise jumps to `0x7C00`. The IVT is installed either way:
  `INT 13h` `AH=0x00` / `0x02` / `0x08` / `0x42` read `DL` `0x80` or
  `0x81` (so a boot sector can load a kernel itself), `INT 10h`
  `AH=0x0E` is a VGA teletype, and `INT 15h` `E820` returns the memory
  map. Any other function on those vectors returns with carry set
- Builds a memory map at `0x0000:0x0500` from the RAM sizes in CMOS (`core/memmap.inc`)
- On success jumps to `0x0000:0x7C00` with `DL = 0x80` or `0x81`,
  `ES:DI = 0x0000:0x0500`

The font is an original design. Its glyphs are drawn in `mkfont.py`,
which writes `font.inc`; after editing a glyph, run
`python3 x86/drivers/vga/mkfont.py > x86/drivers/vga/font.inc` from the repository root. The
build itself does not need Python.

See `../common/handoff.md`.
