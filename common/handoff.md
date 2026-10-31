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
