# Roadmap

This file is where SquirrelBIOS is going. [TODO.md](TODO.md) is the list of
changes that get there, one PR each. What the firmware guarantees today is
[common/handoff.md](common/handoff.md). If the firmware and that file
disagree, the firmware has a bug.

SquirrelBIOS is original firmware for Squirrel-OS/2. It does early init,
loads an OS image, and jumps to it. Nothing in this repo is a runtime
BIOS, UEFI, or a vendored third-party firmware.

## Goals

1. Hand the machine to the OS after early init, on the three QEMU
   machines this repo already builds: PC (`qemu-system-i386`), AArch64
   `virt`, and RISC-V `virt`.
2. Make every guarantee in the hand-off contract something `make test`
   can catch if it breaks.
3. On x86, load a real image. Today the firmware reads one sector to
   `0x7C00` and then offers no disk services, so the OS cannot load the
   rest of itself.
4. On AArch64, enter a Linux `Image` the way the arm64 boot protocol
   describes. The registers and the device tree are already there; the
   CPU state, the image header, and the RAM limit are not finished.
5. On RISC-V, provide an SBI so Linux can boot. QEMU loads the `-kernel`
   image and passes `fw_dynamic_info` (the struct comment in
   `riscv/squirrel-bios.S`). The firmware jumps to `next_addr` in M-mode
   and ignores `next_mode`. There is no SBI. [common/handoff.md](common/handoff.md)
   already says a Linux kernel will not boot this way.

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
  scope until one of these three is in good shape.
- **Failure is a message and a halt.** Match the messages already in the
  contract (`disk: ...`, `boot: ...`, then `boot halted`). Never jump to
  a partial or missing image.

## What works today

Recent work landed the hand-off, not a full boot environment. Leave this
behaviour in place unless a task below says to change it.

| | x86 PC | AArch64 `virt` | RISC-V `virt` |
|---|---|---|---|
| Image | ATA PIO of LBA 0 into `0x7C00`, 3 tries, soft reset between them | `-kernel` via fw_cfg DMA to `0x40200000` | QEMU loads `-kernel`; firmware jumps to `next_addr` |
| Entry | `0x0000:0x7C00`, `DL = 0x80` | `0x40200000`, `x0` = DTB, `x1`–`x3` = 0 | `0x80200000`, M-mode, `a0` = hart, `a1` = DTB |
| Console | COM1 `0x3F8` 115200 8N1, timeouts on the UART | PL011 `0x09000000` | NS16550A `0x10000000` |
| Memory | E820-style map at `0x0500` from CMOS | DTB `/memory`, checked only for the FDT magic | DTB pointer passed through |
| On failure | Missing drive, ATAPI, timeout, bad read: reason plus ATA status/error, then halt | No fw_cfg DMA, no kernel, DMA error: message, then halt | No `fw_dynamic_info`: message, then halt |
| Other CPUs | Boot CPU only | Boot CPU only | Hart 0 boots; other harts sit in `wfi` |

`make test` covers six cases: `x86-boot`, `x86-no-drive`, `arm-boot`,
`arm-no-kernel`, `riscv-boot` (`-smp 2`), `riscv-no-kernel`. The x86
stand-in checks `DL` and that the map has the `'SQMM'` magic and a
non-zero count. It does not check the entries. ATAPI, RAM at most 16 MB,
and RAM above 4 GB are implemented and untested. VGA cells are written
at `0xB8000`, and the VGA is not put into text mode. The AArch64 fw_cfg
DMA poll has no timeout. The RISC-V trap vector is never set.

## Milestones

Do them in this order. Each one is done when its boxes in [TODO.md](TODO.md)
are checked. Speculative ideas at the bottom are not milestones.

### 1. Pin the contract in `make test`

The six smoke cases pass, and several paths they never touch are real
code: the shape of the memory map, the ATAPI reject, the two CMOS RAM
paths, and `a2 = 0` on RISC-V. Lock those in before changing the
hand-off. Add a CI run of `make test` so the later PRs are checked
without a person watching.

### 2. RISC-V: S-mode and an SBI

This is the known gap for Linux.

QEMU's `-kernel` path fills `fw_dynamic_info.next_mode` with S-mode
(`1`). The firmware does not read that field. Dropping to S-mode needs a
PMP entry in the same change: QEMU's CPU implements PMP, and with no
entry programmed an S-mode load or store faults.

The SBI is original code, a thin `ecall` dispatcher, not a copy of
OpenSBI. Implement the calls below and advertise each one from
`probe_extension` only once its task is done. Calling convention: `a7` is
the extension ID, `a6` is the function ID, arguments in `a0`–`a5`; return
the error in `a0` and the value in `a1`; preserve every other register;
advance `mepc` by 4 and `mret`.

| Extension | ID | Functions this milestone needs |
|---|---|---|
| Base | `0x10` | spec version `0x01000000`, impl id, impl version, `probe_extension`, mvendorid, marchid, mimpid |
| TIME | `0x54494D45` | `sbi_set_timer` |
| IPI | `0x735049` | `sbi_send_ipi` |
| RFENCE | `0x52464E43` | remote fence for the running hart |
| HSM | `0x48534D` | `hart_start`, `hart_get_status`, then stop |
| SRST | `0x53525354` | `system_reset` |

Use an implementation ID other than 0 (Berkeley Boot Loader) and 1
(OpenSBI), and record it in the hand-off contract.

A single hart can reach a Linux early print once S-mode, PMP, exception
delegation, Base, and TIME are in. IPI, RFENCE, and HSM are what a second
hart needs. QEMU already placed the image and the initrd and patched the
device tree; this firmware does not copy them.

Supervisor software, timer, and external interrupts are delivered in
S-mode. The CLINT on this machine is `0x02000000` (`mtime` at `+0xBFF8`,
`mtimecmp` at `0x4000 + 8 * hart`). `sbi_hart_start` enters the target
hart in S-mode with `a0` = hart id and `a1` = the opaque argument from
the call. That `a1` is not the device tree unless the caller passed the
device tree as the opaque value. The boot hart is the only one that
receives the device tree in `a1`.

### 3. x86: load more than one sector

Keep the entry at `0x0000:0x7C00` with `DL = 0x80` and the memory map in
`ES:DI`. Refuse a sector whose last word is not `0xAA55`. Then, when
sector 0 carries the header in [TODO.md](TODO.md), read the following
sectors into conventional memory and jump to `0x7C00` as today. The boot
sector is what calls the loaded code. This stays a load-then-jump. It
does not add `INT 13h` or any other runtime disk service.

Addresses stay below 1 MB, in real mode. Loading above 1 MB needs the A20
gate and is speculative.

### 4. AArch64: finish the Linux entry

The jump already matches the register rules (`x0` = DTB, `x1`–`x3` = 0,
MMU left off because QEMU reset left it off). Make that CPU state
explicit, bound the fw_cfg DMA poll the way the UARTs are already
bounded, refuse a kernel that does not fit in RAM, and honour the ARM64
`Image` header when the magic is present. A raw image with no magic,
which is what `tests/arm-kernel.S` is, keeps loading at `0x40200000`.

QEMU's virt machine advertises PSCI in the device tree and implements it
itself. This firmware does not implement PSCI.

### 5. Consoles and small hardening

After the milestones above: a UART baud rate the RISC-V contract states,
CMOS reads that disable NMI while they run, and a VGA text mode so the
x86 banner is actually visible. Serial remains the console `make test`
watches.

### 6. A large server node (future)

Not scheduled. Start it after milestones 1 to 5, when the OS wants more
than a small virtual machine.

A supercomputer is thousands of separate server boards on a fast
network, and each board runs its own firmware. So for SquirrelBIOS the
goal is one big node booting Linux well: tens to hundreds of harts,
tens of GB of RAM or more, and the devices that join it to the rest of
the machine. The firmware stays original code and keeps its
load-then-jump shape; the items below only add what a large node needs.
RISC-V `virt` goes first, because it is the target with an SBI and a
Linux shell already.

`make linux-big` is the yardstick. It boots the `linux-shell` kernel and
busybox on `-smp 64 -m 64G` and passes only when all 64 harts are online.
What it showed on the first run (QEMU 8.2, Ubuntu 6.8 riscv64 kernel):

- **RAM is fine.** Linux sees the full 64 GB from the device tree and
  reaches the shell. QEMU needs `reserve=off` on its RAM backend to start
  a 64 GB guest on a smaller host; the script sets it.
- **Harts 8 and up never started.** `MAX_HARTS` was 8, so Linux printed
  `CPU8: failed to start` and ran on 8. Fixed by `riscv-many-harts`: the
  firmware now tracks 512 harts.
- **The test kernel stopped at 32.** Ubuntu 24.04's riscv64 kernel is
  built with `NR_CPUS=32` and ignores harts 32 to 63 (`Invalid cpuid`).
  Fixed by `linux-big-kernel`: `make linux-big` uses Ubuntu 25.04's 6.14
  kernel, and all 64 harts come up. Ubuntu's 7.0 kernel does not boot on
  QEMU 8.2's default hart: it no longer reads `riscv,isa` and stops
  with a kernel BUG before SMP.

The work, roughly in order:

1. **Many harts.** Raise `MAX_HARTS` to what QEMU `virt` allows, and size
   the per-hart trap stacks and `hart_table` from it without leaving the
   2 MB the firmware PMP region covers. The IPI and fence paths walk
   every hart today; make them cheap when most harts are idle. HSM should
   still bring harts up one at a time on request. Honour
   `fw_dynamic_info.boot_hart` (from Speculative) because large machines
   do not always boot hart 0.
2. **Large memory and NUMA.** Read `/memory` from the device tree instead
   of assuming the QEMU layout, keep the firmware reservation right with
   many memory nodes, and pass through `numa-node-id` and the
   `distance-map` QEMU writes for `-numa`, so Linux knows which RAM is
   near which hart. *(Done: on QEMU each NUMA node is a socket with its
   own CLINT, which the firmware now follows.)*
3. **A sockets-and-clusters topology.** Check that `cpu-map` survives the
   device-tree edits, and that the firmware does not depend on hart IDs
   being dense. *(Checked: the only edit is the `/reserved-memory` node,
   so `cpu-map` passes through as QEMU wrote it. QEMU `virt` numbers
   harts densely in every socket, so sparse IDs moved to Speculative.)*
4. **Interrupt controllers that scale.** QEMU `virt` with `aia=aplic-imsic`
   replaces the PLIC and CLINT-only IPIs with IMSIC message interrupts.
   Program the M-mode side so IPIs and the timer still work there.
5. **PCIe.** The device tree already describes the `virt` host bridge.
   The firmware leaves it to Linux; the work is to check that a network
   card and an NVMe drive enumerate with many harts and lots of RAM.
6. **Network boot.** Thousands of diskless nodes start from the network.
   Load a kernel and initrd over virtio-net (a small original TFTP-style
   loader), or document the hand-off to a kernel that does it itself.
7. **Remote management and error reporting.** A way to power-cycle and
   watch a node from outside (QEMU's monitor and the SBI SRST path are
   the stand-ins), and a place to report machine-check style hardware
   faults instead of a silent halt.
8. **UEFI and ACPI, as an option.** Big ARM and x86 servers expect UEFI
   and ACPI rather than a device tree. That is a separate, large project
   and stays out of scope for the hand-off; if it is ever done it is an
   optional second entry path, not a replacement for this contract.

[TODO.md](TODO.md) breaks the first part into PR-sized items.

## Speculative

Not tasks, until the OS needs one of them. They are listed in
[TODO.md](TODO.md) so they are not mistaken for the milestones above.

- x86 A20, so a load address can sit above 1 MB.
- x86 Linux `bzImage` / setup-header loading.
- x86 ACPI tables.
- x86 disks other than the primary master.
- AArch64 PSCI in the firmware (QEMU already provides it), GIC and timer
  setup (the OS drives these from the device tree).
- RISC-V SBI debug console (`DBCN`), PMU, or vendor extensions.
- Parsing the device tree for the UART and CLINT instead of the QEMU virt
  constants.
- Honouring `fw_dynamic_info.boot_hart` when it is not hart 0 (done
  in milestone 6).
- Checking a Linux or Squirrel-OS image into the repo. `make test` stays
  on the small stand-in images.

## Out of scope

- Runtime firmware services after the jump: x86 `INT 10h` / `INT 13h` /
  `INT 15h`, UEFI, ACPI as a requirement for the hand-off.
- Vendoring another project's firmware and calling it SquirrelBIOS.
- Boards other than QEMU PC, QEMU AArch64 `virt`, and QEMU RISC-V `virt`.
