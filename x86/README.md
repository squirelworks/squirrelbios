# x86 SquirrelBIOS

Current 64 KB ROM for QEMU PC.

```bash
nasm -f bin squirrel-bios.asm -o squirrel-bios.rom
qemu-system-i386 -bios squirrel-bios.rom
```

Reset vector at offset 0xFFF0. Prints a VGA banner and halts.
Next step: load sector 1 of the first disk into 0x7C00 and jump there.
