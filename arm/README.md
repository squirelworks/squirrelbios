# AArch64 SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make arm
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin
```

Prints `SquirrelBIOS ARM` on the PL011 UART (`0x09000000`) then halts.  
Future: device-tree hand-off to an OS entry point.

See `../common/handoff.md`.
