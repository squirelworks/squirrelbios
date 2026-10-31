# AArch64 SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make arm
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin -kernel image.bin
```

- Masks interrupts, enables the PL011 UART (`0x09000000`) and prints `SquirrelBIOS ARM`
- Reads the `-kernel` image through QEMU's fw_cfg DMA interface to `0x40200000`
- Before the jump, at the EL it was started in, clears `SCTLR_ELx` `M`, `C`
  and the endianness bits, keeps `DAIF` masked and invalidates the I-cache
- Jumps to it with `x0` = device tree (`0x40000000`), `x1`–`x3` = 0
- Without `-kernel`, or if the load fails, prints the reason and halts

The image is copied as raw bytes, so build it linked at `0x40200000`.

See `../common/handoff.md`.
