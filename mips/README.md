# MIPS SquirrelBIOS

Minimal firmware for QEMU `malta` (big-endian MIPS32, roadmap E3, stretch).

```bash
make mips
python3 mips/mkimage.py mips/squirrel-bios.bin kernel.bin flash.img --cmdline "..."
qemu-system-mips -M malta -nographic -bios flash.img
```

- Starts at the reset vector `0xbfc00000` and works through kseg1
  (uncached) only; before the hand-off it makes kseg0 uncached as well
  (`Config.K0` = 2), since it never sets the caches up
- Prints `SquirrelBIOS MIPS (malta)` on the PIIX4's COM1 (port `0x3f8` in
  PCI I/O space, at `0x10000000` as the GT-64120 leaves it after reset)
- Measures RAM a megabyte at a time (up to 256 MB)
- QEMU's `-kernel` on Malta replaces the boot flash's contents, so the
  kernel is stored in the flash after the firmware: `mips/mkimage.py` puts
  it at 1 MB with a header (`SQKN`, size, load and entry addresses, a
  256-byte command line). The firmware copies it to its load address (a
  kseg0 address in RAM, default `0x80100000`)
- CP0 Count, at QEMU's 160 MHz, times the 2-second autoboot, which any
  key stops
- The prompt `sq> ` takes `help`, `info` and `boot` (backspace works)
- Hand-off the way a YAMON monitor starts a kernel: `a0` = argc, `a1` =
  argv (`squirrelbios` and the command line, if any), `a2` = envp
  (`memsize` and its value in bytes), `a3` = RAM bytes; all pointers are
  kseg0 addresses

`tests/mips-kernel.S` is the test kernel. `make test` runs the cases only
when `mips-linux-gnu-` binutils and `qemu-system-mips` are installed;
otherwise it prints SKIP for them.
