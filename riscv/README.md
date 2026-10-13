# RISC-V SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin
```

Prints `SquirrelBIOS RISC-V` on the UART (`0x10000000`) then halts.  
Future: device-tree hand-off to an OS entry point.

See `../common/handoff.md`.
