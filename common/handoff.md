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

The firmware loads the image QEMU was given with `-kernel`, read through
fw_cfg DMA, and jumps to its first byte. The register state follows the
Linux arm64 boot protocol, so a raw Linux `Image` can be booted the same way.

| Item | Value |
|------|--------|
| Entry | `0x40200000` (RAM base + 2 MB), first byte of the image |
| Exception level | Whatever QEMU started the firmware at: EL1 by default, EL2 with `virtualization=on` |
| MMU, caches | MMU off, D-cache off. No page tables are built. |
| Interrupts | `DAIF` all masked |
| `x0` | Device tree address (`0x40000000`) if a valid FDT is there, otherwise `0` |
| `x1`–`x3` | `0` |
| Stack | `SP` points into firmware scratch below `0x40200000`. Set your own. |
| Console | PL011 at `0x09000000`, enabled (`UARTEN`, `TXE`, `RXE`) |

Memory at hand-off:

| Range | Contents |
|-------|----------|
| `0x40000000`–`0x400FFFFF` | Device tree from QEMU (keep it until you have parsed it) |
| `0x40100000`–`0x401FFFFF` | Firmware stack and scratch, free after the jump |
| `0x40200000`–… | The image, `-kernel` size bytes |

RAM size and layout come from the device tree (`/memory`). On failure the
firmware prints one of these on the PL011 and halts:

| Message | Meaning |
|---------|---------|
| `fw_cfg with DMA not found` | Not on QEMU virt, or a QEMU too old for fw_cfg DMA |
| `no kernel image (pass -kernel to QEMU)` | fw_cfg reports a kernel size of 0 |
| `fw_cfg DMA read of the kernel failed` | The DMA transfer reported an error |

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
