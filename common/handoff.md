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

QEMU loads the `-kernel` image into RAM itself and tells the firmware
where through the `fw_dynamic_info` block it passes in `a2` (magic
`"OSBI"`, `next_addr`). The firmware jumps there without changing
privilege.

| Item | Value |
|------|--------|
| Entry | `next_addr` from QEMU, `0x80200000` on rv64 (RAM base + 2 MB) |
| Privilege | M-mode. There is no SBI: the OS owns `mtvec`, the PMP and all traps. |
| Harts | Hart 0 only. Other harts park in `wfi` with interrupts off and are not released. |
| Interrupts | `mstatus.MIE = 0`, `mie = 0` |
| MMU | `satp` untouched (bare after reset) |
| `a0` | Hart ID (0) |
| `a1` | Device tree address, as QEMU passed it to the firmware |
| `a2` | `0` |
| Stack | `SP` points into the firmware image. Set your own. |
| Console | NS16550A at `0x10000000`, 8N1, FIFOs on, interrupts off |

The firmware itself occupies RAM from `0x80000000` up to the image. RAM
size and layout come from the device tree (`/memory`). Without `-kernel`
the firmware prints `boot: no kernel image (pass -kernel to QEMU)` and halts.

A Linux kernel will not boot this way: it expects S-mode and an SBI.

## Common rules

1. Firmware performs only early init and the load/jump.
2. The OS must not rely on firmware interrupt handlers after hand-off.
3. A console device is either already printing or has a known base address.
4. Drive/image identity is passed in an architecture-defined register or is implicit for the first drive.
