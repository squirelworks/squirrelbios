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
| Console | COM1 (`0x3F8`) programmed 115200 8N1, UART interrupts off |
| Disk | Primary ATA channel idle, `nIEN` may be set if a retry reset the channel |

The OS owns the machine after the jump. No further firmware calls are required.

The jump only happens after a clean read: the drive reported no `ERR`/`DF`,
delivered exactly one sector and went idle. On failure the firmware does
not jump. It prints the reason and the ATA status and error registers to
both the VGA text buffer and COM1, then halts:

| Message | Meaning |
|---------|---------|
| `no drive on primary master` | Floating bus or nothing answered as master. Not retried. |
| `primary master is ATAPI (CD/DVD), not supported` | ATAPI signature found. Not retried. |
| `timeout waiting for drive` | `BSY` never cleared or the expected bit never came up |
| `read of LBA 0 failed` | Drive set `ERR`/`DF`, or the transfer ended in a bad state |

Timeouts and read errors are retried up to 3 times, with an ATA soft reset
(`SRST`) between attempts.

The firmware does not program the VGA into text mode or load a font, so
on QEMU the VGA window stays blank and COM1 is the console to watch
(`-serial stdio` or `-nographic`).

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
