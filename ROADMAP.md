# Roadmap

This file is where SquirrelBIOS is going. [TODO.md](TODO.md) is the list of
changes that get there, one PR each. What the firmware guarantees today is
[common/handoff.md](common/handoff.md). If the firmware and that file
disagree, the firmware has a bug.

SquirrelBIOS is original firmware for Squirrel-OS/2 and for the QEMU
machines this repo builds. **v0.3.0** closed the hand-off era: early init,
load an image, jump, with a tested contract on x86 PC, AArch64 `virt`, and
RISC-V `virt`. **v1.0.0** is a comprehensive **x86 PC BIOS** on QEMU —
runtime services an OS and boot sector actually call — not a joke thin
stub and not a copy of SeaBIOS/EDK2.

Nothing in this repo is UEFI, and nothing is vendored third-party
firmware.

## Goals

### Done (v0.3.0 — hand-off)

1. Hand the machine to the OS after early init on QEMU PC, AArch64
   `virt`, and RISC-V `virt`.
2. Every guarantee in the hand-off contract is something `make test` can
   catch if it breaks.
3. x86 loads real images (`0xAA55`, `SQBL`, `S2OK`) and serves enough
   `INT 13h` / `INT 10h` / `INT 15h` E820 for Squirrel-OS/2's boot sector.
4. AArch64 enters a Linux `Image` per the arm64 boot protocol (with
   initrd in the device tree when QEMU provides one).
5. RISC-V provides an original SBI so Linux boots (including a large
   node: many harts, NUMA, AIA aplic, PCIe smoke, RAS shutdown).

### v1.0.0 — comprehensive x86 PC BIOS

On QEMU's PC machine (`qemu-system-i386` / `x86_64` as needed), the
firmware is a real BIOS the OS keeps calling after boot:

1. **POST-shaped init.** Program the PIC and PIT, open A20, fill the
   BIOS data area, leave keyboard and timer interrupts usable.
2. **Runtime interrupt services.** Grow the IVT beyond today's three
   vectors: keyboard (`INT 16h`), time (`INT 1Ah`), richer video text
   (`INT 10h`), disk writes and more disk functions (`INT 13h`), and the
   usual `INT 15h` helpers (A20, etc.) on top of E820.
3. **System tables.** Build ACPI (and SMBIOS) tables the OS can find,
   without making UEFI a requirement.
4. **Boot paths.** Keep Squirrel-OS/2 and `SQBL`; add Linux `bzImage`
   load; scan option ROMs; try more than the primary master when
   present.
5. **Still original, still tested.** Every new service lands with a
   `make test` stand-in and a contract update. Do not vendor SeaBIOS,
   EDK2, or another BIOS.

AArch64 and RISC-V stay supported platform firmware (Linux hand-off and
SBI). Their extra polish lives on a secondary track and does not block
x86 v1.0.0.

## Principles

- **Original code only.** New firmware is written in this repo. Do not
  import or vendor OpenSBI, U-Boot, SeaBIOS, EDK2, or another firmware.
- **Small steps.** One item in [TODO.md](TODO.md) is one PR. The PR
  includes the `make test` check that proves it.
- **Docs move with the code.** The same PR updates `common/handoff.md`,
  and the architecture README when the visible behaviour changes. Check
  the item's box in `TODO.md` in that PR.
- **Branch from `main`.** Every PR targets `main`. Stacked PRs are not
  used. Merge, then start the next item from the new `main`.
- **QEMU is the machine.** Addresses, CMOS, fw_cfg, and the UARTs are the
  ones these three QEMU machines already use. A second board is out of
  scope until the PC BIOS path is in good shape.
- **Failure is a message and a halt.** Match the messages already in the
  contract (`disk: ...`, `boot: ...`, then `boot halted`). Never jump to
  a partial or missing image.
- **x86 keeps serving.** After the jump, x86 firmware services stay
  available through the IVT. That is the point of v1.0.0. RISC-V keeps
  the SBI; AArch64 remains load-then-jump unless a later milestone says
  otherwise.

## What works today (v1.0.0)

| | x86 PC | AArch64 `virt` | RISC-V `virt` |
|---|---|---|---|
| Image | ATA PIO: master or slave; `SQBL` / `S2OK` / `bzImage` (`HdrS`) | `-kernel` via fw_cfg DMA; `Image` header; optional `-initrd` in `/chosen` | QEMU loads `-kernel`; S-mode entry when `next_mode` says so |
| Entry | `0x7C00`, `0x8000` (`S2OK`), or PM `code32_start` (`bzImage`). `DL = 0x80`/`0x81` | Image entry (often `0x40200000`), `x0` = DTB, `x1`–`x3` = 0 | `next_addr`, S-mode, `a0` = hart, `a1` = DTB |
| Console | COM1 `0x3F8` 115200 8N1; VGA 80x25 text | PL011 `0x09000000` | NS16550A `0x10000000` 115200 8N1 |
| Memory | SQMM map at `0x0500` from CMOS; `INT 15h` E820 | DTB `/memory` | DTB `/memory`; firmware `no-map` reserved |
| Firmware services | `INT 08h`/`09h`/`10h`/`13h`/`15h`/`16h`/`1Ah`; ACPI; SMBIOS; option ROMs | None after jump | SBI: base, TIME, IPI, RFENCE, HSM, SRST |
| Other CPUs | Boot CPU only | Boot CPU only | Boot hart (+ `sbi_hart_start`); up to 512 harts tracked |

`make test` is the CI gate (58 cases at v1.0.0). `make linux-shell` /
`make linux-big` are optional RISC-V Linux checks.

## Milestones (done through v1.0.0)

Hand-off milestones 1–7 and x86 PC BIOS milestones 8–12 are finished
and stay checked in [TODO.md](TODO.md) as history.

### 8. x86 foundations (POST and A20)

Fill the BIOS data area the way a PC BIOS does for the fields boot code
reads, enable the A20 line, and program the PIT and PIC so timer and
keyboard IRQs can be used once their handlers exist. Without this, later
services fight QEMU's reset defaults.

### 9. x86 human interface and time

`INT 16h` keyboard (at least check/read keystroke) and `INT 1Ah` time of
day / RTC, with IRQ handlers that keep the BDA counters honest. Boot
sectors and DOS-alike code expect these.

### 10. x86 richer disk and video

Grow `INT 13h` (writes, reset edge cases, primary slave when the master
is absent) and `INT 10h` (cursor, scroll, mode query/set for text — not
a full VESA graphics BIOS). Extend `INT 15h` for A20 control alongside
E820.

### 11. x86 system tables and high loaders

ACPI tables the OS can find (RSDP checksum in a stand-in). SMBIOS where
it helps inventory. Linux `bzImage` load once A20 is on. Option ROM
scan in the legacy regions. These are what make the firmware look like a
real PC to modern and classic loaders.

### 12. v1.0.0 release

When milestones 8–11's TODO boxes are checked: release notes, tag
`v1.0.0`, and a contract that lists the full IVT surface. No SeaBIOS
inside.

## Secondary track (does not block v1.0.0)

- RISC-V: `DBCN`, device-tree-derived UART/CLINT addresses, SSE once
  QEMU can inject corrected errors, `aplic-imsic` when the hypervisor
  stack allows a shell.
- AArch64: firmware PSCI/GIC only if QEMU's built-ins are disabled for a
  test; otherwise leave them.

## Out of scope

- UEFI as the hand-off (ACPI tables without UEFI are in scope for
  milestone 11).
- Vendoring SeaBIOS, EDK2, OpenSBI, U-Boot, or another project's
  firmware and calling it SquirrelBIOS.
- A full VESA/SVGA video BIOS and option-ROM VGA BIOS replacement.
- Boards other than QEMU PC, QEMU AArch64 `virt`, and QEMU RISC-V `virt`
  until the PC BIOS path is in good shape.
