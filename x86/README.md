# x86 SquirrelBIOS

64 KB ROM for QEMU PC.

```bash
nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
qemu-system-i386 -bios squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide
```

- Reset vector at offset 0xFFF0
- Prints banner to VGA
- ATA PIO reads LBA 0 into `0x7C00`
- Jumps to `0x0000:0x7C00` with `DL = 0x80`

See `../common/handoff.md`.
