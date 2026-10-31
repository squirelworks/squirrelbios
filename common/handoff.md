# Hand-off contract

This file is the single source of truth for what SquirrelBIOS guarantees
when it passes control to the OS, and for what the OS must not assume.
If the firmware and this file disagree, the firmware has a bug.

## At a glance

| | x86 PC | AArch64 (QEMU virt) | RISC-V (QEMU virt) |
|---|---|---|---|
| Image source | LBA 0 of the primary master ATA disk | `-kernel`, read through fw_cfg DMA | `-kernel`, loaded by QEMU |
| Entry | `0x0000:0x7C00` | `0x40200000` | `0x80200000` (rv64) |
| CPU mode | 16-bit real mode | EL1 (EL2 with `virtualization=on`), MMU off | S-mode (QEMU's `next_mode`), `satp` bare |
| Boot argument | `DL = 0x80` | `x0` = device tree | `a0` = hart ID, `a1` = device tree |
| Memory description | Memory map at `ES:DI = 0x0000:0x0500` | Device tree `/memory` | Device tree `/memory` |
| Console | COM1 `0x3F8`, 115200 8N1 | PL011 `0x09000000` | NS16550A `0x10000000` |
| Interrupts | `IF = 0` | `DAIF` masked | `mstatus.MIE`/`SIE` = 0, `mie = 0` |
| CPUs running | Boot CPU only | Boot CPU only | Hart 0 only |

## Rules for every architecture

The firmware guarantees:

1. It does only early init, the image load and the jump. Nothing runs
   after the jump.
2. Interrupts are masked at the CPU on entry.
3. The serial console is initialised and has already printed the banner,
   so the OS can write to it straight away.
4. If the image cannot be loaded, the firmware prints the reason on the
   console and halts. It never jumps to a partial or missing image.

The OS must not assume:

1. **Firmware services.** There are none after hand-off: no BIOS
   interrupts on x86 (`INT 10h`, `INT 13h`, `INT 15h` do not exist), no
   UEFI, no SBI. Anything the OS needs, it drives itself.
2. **A usable stack.** Set your own stack before using it heavily.
3. **Trap or interrupt vectors.** Install your own before unmasking
   interrupts.
4. **Register values not listed below.** Treat them as undefined.
5. **That firmware data stays put.** Hand-off data (the x86 memory map,
   the device tree) lives in RAM the OS may reuse. Copy or parse it before
   overwriting that RAM.
6. **Other CPUs.** Only the boot CPU runs the OS. Secondary CPUs are not
   started (AArch64) or are parked and never released (RISC-V).

## x86 PC

### Registers

| Item | Value |
|------|-------|
| `CS:IP` | `0x0000:0x7C00` (far jump) |
| Mode | 16-bit real mode |
| `DS`, `ES`, `SS` | `0` |
| `SP` | `0x7C00` |
| `DL` | `0x80` (boot drive) |
| `DI` | `0x0500` (memory map header, so `ES:DI` points at it) |
| `EFLAGS` | `IF = 0`, `DF = 0` |

### Memory

| Range | Contents |
|-------|----------|
| `0x0500`–`0x05FF` | Memory map (below). Always at this address. |
| below `0x7C00` | Firmware stack, a few bytes, free after the jump |
| `0x7C00`–`0x7DFF` | Sector 1 of the disk, the image that was jumped to |

### Devices

| Device | State |
|--------|-------|
| COM1 (`0x3F8`) | 115200 8N1, FIFOs on, UART interrupts off |
| Primary ATA channel | Idle. `nIEN` is set if a retry reset the channel. |
| VGA | Not initialised. Text was written to `0xB8000`, but the VGA is not in text mode and has no font, so on QEMU nothing shows. |

Do not assume: BIOS interrupt services, an initialised IVT, A20 state
(enabled on QEMU, but the firmware does not touch it), or a working VGA
text console.

### Memory map

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

### Failures

The jump only happens after a clean read: the drive reported no `ERR`/`DF`,
delivered exactly one sector and went idle. Timeouts and read errors are
retried up to 3 times, with an ATA soft reset (`SRST`) between attempts.
On failure the firmware prints the reason and the ATA status and error
registers on COM1 (and the VGA text buffer), then halts:

| Message | Meaning |
|---------|---------|
| `no drive on primary master` | Floating bus or nothing answered as master. Not retried. |
| `primary master is ATAPI (CD/DVD), not supported` | ATAPI signature found. Not retried. |
| `timeout waiting for drive` | `BSY` never cleared or the expected bit never came up |
| `read of LBA 0 failed` | Drive set `ERR`/`DF`, or the transfer ended in a bad state |

### How to test

`make test` runs `x86-boot`, `x86-no-drive`, `x86-mem-low`,
`x86-mem-high` and `x86-atapi`. In `x86-boot` the boot sector checks
`DL`, that `IF` and `DF` are clear, and that the memory map has the
magic, 20-byte entries, ends by `0x0600`, contains the conventional and
both reserved regions above exactly, and is sorted without overlaps. It
then lists every entry on COM1 as `mm <base> <length> <type>`, which
`x86-mem-low` uses to check the extended-memory entry for `-m 8` and
`x86-mem-high` to check the entry at 4 GB for `-m 5120`. By hand:

```bash
make x86
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio
```

Expect `SquirrelBIOS x86` on the terminal, then whatever `boot.bin`
prints. `tests/x86-boot.asm` is a minimal boot sector that reports the
hand-off state on COM1.

## AArch64 (QEMU virt)

The firmware loads the image QEMU was given with `-kernel`, read through
fw_cfg DMA, and jumps to its first byte. The register state follows the
Linux arm64 boot protocol, so a raw Linux `Image` can be booted the same way.

### Registers

| Item | Value |
|------|-------|
| `PC` | `0x40200000` (RAM base + 2 MB), first byte of the image |
| Exception level | Whatever QEMU started the firmware at: EL1 by default, EL2 with `virtualization=on` |
| MMU, D-cache | Off. No page tables are built. |
| `DAIF` | All masked |
| `x0` | Device tree address (`0x40000000`) if a valid FDT is there, otherwise `0` |
| `x1`–`x3` | `0` |
| `SP` | Inside firmware scratch below `0x40200000` |

### Memory

| Range | Contents |
|-------|----------|
| `0x40000000`–`0x400FFFFF` | Device tree from QEMU |
| `0x40100000`–`0x401FFFFF` | Firmware stack and scratch, free after the jump |
| `0x40200000`–… | The image, `-kernel` size bytes |

RAM size and layout come from the device tree (`/memory`).

### Devices

| Device | State |
|--------|-------|
| PL011 (`0x09000000`) | Enabled (`UARTEN`, `TXE`, `RXE`), baud rate as QEMU left it |
| GIC, timers | Not touched |

Do not assume: I-cache state, a configured GIC or timer, PSCI beyond what
the device tree advertises, or that the image was relocated or
decompressed. It is copied as raw bytes, so link it at `0x40200000`.

### Failures

The firmware prints one of these on the PL011, then `boot halted`, and halts:

| Message | Meaning |
|---------|---------|
| `fw_cfg with DMA not found` | Not on QEMU virt, or a QEMU too old for fw_cfg DMA |
| `no kernel image (pass -kernel to QEMU)` | fw_cfg reports a kernel size of 0 |
| `fw_cfg DMA read of the kernel failed` | The DMA transfer reported an error |

### How to test

`make test` runs `arm-boot` (an image checks `x0`–`x3` and the device
tree) and `arm-no-kernel`. By hand:

```bash
make arm
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin -kernel image.bin
```

Expect `SquirrelBIOS ARM`, then `kernel loaded at 0x40200000, jumping`,
then whatever the image prints. `tests/arm-kernel.S` is a minimal image.

## RISC-V (QEMU virt)

QEMU loads the `-kernel` image into RAM itself and tells the firmware
where through the `fw_dynamic_info` block it passes in `a2` (magic
`"OSBI"`, `next_addr`, `next_mode`). The firmware enters the image in
the mode QEMU asks for. With `-kernel` that is S-mode (`next_mode = 1`),
entered through `mret`.

### Registers

| Item | Value |
|------|-------|
| `pc` | `next_addr` from QEMU, `0x80200000` on rv64 (RAM base + 2 MB) |
| Privilege | S-mode when `next_mode` is `1` (what `-kernel` gives), M-mode when it is `3`, or when `fw_dynamic_info` is older than version 2 and has no `next_mode` |
| PMP | S-mode entry: entry 0 is NAPOT over the whole physical address space with R, W and X, unlocked. Other entries are untouched. M-mode entry: untouched. |
| `satp` | Untouched (bare after reset) |
| `mtvec` | Firmware trap handler, direct mode (below) |
| `mstatus.MIE`, `mstatus.SIE`, `mie` | `0` |
| `a0` | Hart ID (`0`) |
| `a1` | Device tree address, as QEMU passed it to the firmware |
| `a2` | `0` |
| `sp` | Inside the firmware image |

### Memory

| Range | Contents |
|-------|----------|
| `0x80000000`–`0x801FFFFF` | Firmware image and stack, free after the jump |
| `0x80200000`–… | The image |
| `a1` | Device tree, placed by QEMU near the top of RAM |

RAM size and layout come from the device tree (`/memory`).

### Devices and harts

| Item | State |
|------|-------|
| NS16550A (`0x10000000`) | 8N1, FIFOs on, interrupts off |
| Other harts | Parked in `wfi` with interrupts off. They are never released. |
| CLINT, PLIC | Not touched |

Do not assume: an SBI (there is none, so a Linux kernel will not boot
this way), delegated traps (`medeleg` and `mideleg` are untouched, so
every S-mode trap goes to the firmware handler below), or secondary harts
being available.

### Traps

`mtvec` points at a firmware handler, so a trap taken before the OS
installs its own vector is reported instead of jumping to address 0. In
S-mode nothing is delegated yet, so every S-mode trap, `ecall` included,
reaches this handler. The
handler does not return. It prints `trap mcause=` and the cause as 16
lowercase hex digits (no `0x`), then `boot halted`, and halts. It reuses
the firmware stack. Do not rely on traps until there is an SBI and
delegation: today an `ecall` from S-mode (cause `0000000000000009`) or an
illegal instruction halts the machine. An image entered in M-mode
installs its own `mtvec` first (an M-mode `ecall` is cause
`000000000000000b`).

### Failures

The firmware prints one of these on the UART, then `boot halted`, and halts:

| Message | Meaning |
|---------|---------|
| `boot: no kernel image (pass -kernel to QEMU)` | No `-kernel`, or QEMU passed no valid `fw_dynamic_info` |
| `boot: unsupported next_mode <16 hex digits>` | `next_mode` is not `1` (S) or `3` (M), for example U-mode (`0`) |

### How to test

`make test` runs `riscv-boot` (the firmware reports `next_mode=1`, and an
image checks `a0`, the device tree in `a1` and `a2 = 0`, with `-smp 2`;
its UART stores and device-tree load pass PMP in S-mode, and the log must
not show a trap), `riscv-ecall` (an image executes `ecall` and the handler
reports `trap mcause=0000000000000009`, environment call from S-mode) and
`riscv-no-kernel`. By hand:

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

Expect `SquirrelBIOS RISC-V`, then `jumping to kernel at
0x0000000080200000, next_mode=1`, then whatever the image prints.
`tests/riscv-kernel.S` is a minimal image.
