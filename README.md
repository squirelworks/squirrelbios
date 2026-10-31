# SquirrelBIOS

Platform-specific firmware for Squirrel-OS/2.  
Original code. Hands off to the OS after early init.

```
x86/      64 KB ROM — VGA banner + ATA hand-off to 0x7C00
arm/      AArch64 QEMU virt — PL011 banner
riscv/    RISC-V QEMU virt — UART banner
common/   hand-off contract
```

## Build

```bash
make          # all
make x86
make arm
make riscv
```

Requires: `nasm`, `aarch64-linux-gnu-as`/`ld`/`objcopy`, `riscv64-linux-gnu-as`/`ld`/`objcopy`.

## Run

```bash
# x86 (with a boot sector image); firmware messages appear on COM1
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio

# AArch64
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin

# RISC-V
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin
```

See `common/handoff.md` for the contract each architecture provides.
