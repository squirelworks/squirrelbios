# LoongArch64 SquirrelBIOS

Minimal firmware for QEMU `virt` (roadmap E3, stretch).

```bash
make loongarch
qemu-system-loongarch64 -M virt -nographic -bios loongarch/squirrel-bios.bin -kernel image.bin -append "..."
```

- Runs from flash at `0x1c000000` in direct address mode (no paging; all
  accesses strongly ordered and uncached), stack and buffers in the first
  MB of RAM
- Prints `SquirrelBIOS LoongArch64 (virt)` on the NS16550 at `0x1fe001e0`
- Reads through QEMU's fw_cfg (MMIO at `0x1e020000`): the RAM size, the
  `-kernel` image (copied as raw bytes to `0x00200000`; it must fit below
  256 MB) and the `-append` line
- The timer frequency comes from CPUCFG words 4 and 5; `rdtime.d` counts
  the 2-second autoboot, which any key stops
- The prompt `sq> ` takes `help`, `info` and `boot` (backspace works)
- Hand-off: jumps to `0x00200000` with `a0` = RAM bytes, `a1` = the
  command line (NUL-terminated) or 0, `a2` = 0

Build the kernel linked at `0x00200000`. `tests/loongarch-kernel.S` is the
test kernel. `make test` runs the cases only when `loongarch64-linux-gnu-`
binutils and `qemu-system-loongarch64` are installed; otherwise it prints
SKIP for them.
