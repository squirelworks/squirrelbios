# x86 SquirrelBIOS

64 KB ROM for QEMU PC.

```bash
nasm -f bin squirrel-bios.asm -o squirrel-bios.rom   # run from x86/, or pass -i x86/
qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio
```

- Reset vector at offset 0xFFF0
- Programs COM1 at 115200 8N1; every message goes to VGA text and COM1
- Prints banner
- ATA PIO reads LBA 0 into `0x7C00`, with timeouts on every status wait,
  `ERR`/`DF` checks, and up to 3 attempts with a soft reset in between
- Detects a missing drive or an ATAPI device and says so instead of hanging
- On failure prints the reason and the ATA status/error registers, then halts
- Builds a memory map at `0x0000:0x0500` from the RAM sizes in CMOS (`memmap.inc`)
- On success jumps to `0x0000:0x7C00` with `DL = 0x80`, `ES:DI = 0x0000:0x0500`

The VGA is not put into text mode (no font is loaded), so under QEMU use
COM1 to see the output.

See `../common/handoff.md`.
