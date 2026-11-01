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
- Prints banner
- ATA PIO reads LBA 0 into `0x7C00`, with timeouts on every status wait,
  `ERR`/`DF` checks, and up to 3 attempts with a soft reset in between
- Detects a missing drive or an ATAPI device and says so instead of hanging
- On failure prints the reason and the ATA status/error registers, then halts
- Refuses a sector whose last word is not `0xAA55` (not retried), then halts
- When the sector has an SQBL header at `0x1F0`, reads the 1–128 sectors
  after it to the header's `segment:offset` (below `0xA0000`, clear of the
  memory map and the boot sector) before jumping to `0x7C00`
- When it does not, and LBA 192 ends in `S2OK`, reads LBAs 1–192 to
  `0x8000` and jumps there. The boot sector is not entered
- Otherwise jumps to `0x7C00`. The IVT is installed either way:
  `INT 13h` `AH=0x00` / `0x02` / `0x08` / `0x42` read the primary
  master (so a boot sector can load a kernel itself), `INT 10h`
  `AH=0x0E` is a VGA teletype, and `INT 15h` `E820` returns the memory
  map. Any other function on those vectors returns with carry set
- Builds a memory map at `0x0000:0x0500` from the RAM sizes in CMOS (`memmap.inc`)
- On success jumps to `0x0000:0x7C00` with `DL = 0x80`, `ES:DI = 0x0000:0x0500`

The font is an original design. Its glyphs are drawn in `mkfont.py`,
which writes `font.inc`; after editing a glyph, run
`python3 x86/mkfont.py > x86/font.inc` from the repository root. The
build itself does not need Python.

See `../common/handoff.md`.
