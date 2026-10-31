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
| Memory map | `ES:DI = 0x0000:0x0500` (header below, always at this address) |
| Memory | Sector 1 loaded at `0x7C00` |
| Interrupts | Disabled on entry (`CLI` before jump) |

The OS owns the machine after the jump. No further firmware calls are required.

### x86 memory map

There is no `INT 15h, E820` after hand-off, so the firmware builds the
same kind of table in memory before jumping. It is derived from the RAM
sizes QEMU stores in CMOS (`0x30/0x31`, `0x34/0x35`, `0x5B`–`0x5D`).

| Offset | Size | Field |
|--------|------|-------|
| `0x0500` | 4 | Magic `'SQMM'` (bytes `53 51 4D 4D`) |
| `0x0504` | 2 | Entry count |
| `0x0506` | 2 | Entry size in bytes (20) |
| `0x0508` | 20 × count | Entries |

Each entry uses the E820 layout: 64-bit base, 64-bit length, 32-bit type
(`1` usable, `2` reserved). Entries are sorted by base and do not overlap.

| Range | Type | Present |
|-------|------|---------|
| `0x00000000`–`0x0009FFFF` | usable | always |
| `0x000A0000`–`0x000FFFFF` | reserved (VGA window, ROMs) | always |
| `0x00100000`–top of low RAM | usable | when RAM > 1 MB |
| `0xFEC00000`–`0xFFFFFFFF` | reserved (IOAPIC, LAPIC, firmware) | always |
| `0x100000000`–… | usable | when RAM is mapped above 4 GB |

The table lives in usable memory. Copy it, or check the magic first, before
reusing `0x0500`–`0x05FF`. Memory not covered by an entry (for example the
PCI hole below 4 GB) is not RAM. Do not assume it is free.

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
