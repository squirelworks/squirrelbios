# RISC-V SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

- Hart 0 boots; other harts park in `wfi`
- Initialises the NS16550A UART (`0x10000000`) and prints `SquirrelBIOS RISC-V`
- Finds the `-kernel` image QEMU loaded (via the `fw_dynamic_info` block in `a2`)
- Points `mtvec` at a handler that prints `trap mcause=<16 hex digits>`, then `boot halted`, and halts
- Jumps to it in M-mode with `a0` = hart ID, `a1` = device tree
- Without `-kernel`, prints the reason and halts

QEMU puts the image at `0x80200000` on rv64, so link it there.

See `../common/handoff.md`.
