# Hand-off contract

SquirrelBIOS hands control to the OS with a minimal, documented state.

## x86 PC

| Item | Value |
|------|--------|
| Entry | `0x0000:0x7C00` (far jump) |
| Mode | 16-bit real mode |
| Segments | `DS = ES = SS = 0` |
| Stack | `SP = 0x7C00` |
| Boot drive | `DL = 0x80` |
| Memory | Sector 1 loaded at `0x7C00` |
| Interrupts | Disabled on entry (`CLI` before jump) |

The OS owns the machine after the jump. No further firmware calls are required.

## AArch64 (QEMU virt)

| Item | Value |
|------|--------|
| Entry | Start of image (EL1) |
| Console | PL011 at `0x09000000` already usable for output |
| Stack | Set by firmware before any future jump |
| Arguments | None in the current scaffold (future: device-tree pointer in `x0`) |

## RISC-V (QEMU virt)

| Item | Value |
|------|--------|
| Entry | Start of image |
| Console | UART at `0x10000000` already usable for output |
| Stack | Set by firmware before any future jump |
| Arguments | None in the current scaffold (future: device-tree pointer in `a1`) |

## Common rules

1. Firmware performs only early init and the load/jump.
2. The OS must not rely on firmware interrupt handlers after hand-off.
3. A console device is either already printing or has a known base address.
4. Drive/image identity is passed in an architecture-defined register or is implicit for the first drive.
