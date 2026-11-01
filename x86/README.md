# x86 SquirrelBIOS

64 KB ROM for QEMU PC.

```bash
nasm -f bin squirrel-bios.asm -o squirrel-bios.rom   # run from x86/, or pass -i x86/
qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio
```

- Reset vector at offset 0xFFF0
- Programs COM1 at 115200 8N1
- Puts the VGA in 80x25 colour text mode and loads its own 8x16 font
  (`vga.inc`, `font.inc`); every message goes to the screen and COM1
- Fills the BIOS data area equipment list (`0x0410` = `0x0220`),
  conventional memory KB (`0x0413` = 640), and page-0 cursor (`0x0450`)
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
- Places a SMBIOS 2.4 entry point at `0xFD000` (`smbios.inc`) and a
  minimal ACPI 1.0 RSDP/RSDT/FADT at `0xFE000` (`acpi.inc`)
- Detects a Linux `bzImage` (`HdrS`), loads the PM kernel at 1 MB, and
  enters it in 32-bit protected mode with `ESI` = boot_params
- Prints banner
- ATA PIO reads LBA 0 into `0x7C00`, with timeouts on every status wait,
  `ERR`/`DF` checks, and up to 3 attempts with a soft reset in between;
  if the primary master is absent, retries the same read on the slave
- Detects a missing drive or an ATAPI device and says so instead of hanging
- On failure prints the reason and the ATA status/error registers, then halts
- Refuses a sector whose last word is not `0xAA55` (not retried), then halts
- When the sector has an SQBL header at `0x1F0`, reads the 1–128 sectors
  after it to the header's `segment:offset` (below `0xA0000`, clear of the
  memory map and the boot sector) before jumping to `0x7C00`
- When it does not, and LBA 192 ends in `S2OK`, reads LBAs 1–192 to
  `0x8000` and jumps there. The boot sector is not entered
- Otherwise jumps to `0x7C00`. The IVT is installed either way:
  `INT 13h` `AH=0x00` / `0x02` / `0x08` / `0x42` read `DL` `0x80` or
  `0x81` (so a boot sector can load a kernel itself), `INT 10h`
  `AH=0x0E` is a VGA teletype, and `INT 15h` `E820` returns the memory
  map. Any other function on those vectors returns with carry set
- Builds a memory map at `0x0000:0x0500` from the RAM sizes in CMOS (`memmap.inc`)
- On success jumps to `0x0000:0x7C00` with `DL = 0x80` or `0x81`,
  `ES:DI = 0x0000:0x0500`

The font is an original design. Its glyphs are drawn in `mkfont.py`,
which writes `font.inc`; after editing a glyph, run
`python3 x86/mkfont.py > x86/font.inc` from the repository root. The
build itself does not need Python.

See `../common/handoff.md`.
