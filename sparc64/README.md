# SPARC SquirrelBIOS (`sparc64-sun4u`, roadmap E2)

Firmware for QEMU's `sun4u` machine (UltraSPARC-IIi), in C with an
assembly reset path and trap table. It gives the OS an IEEE 1275-style
client interface over a device tree, without Forth.

```bash
make sparc64          # needs sparc64-linux-gnu-gcc (gcc-sparc64-linux-gnu)
qemu-system-sparc64 -M sun4u -nographic -bios sparc64/squirrel-bios.bin -kernel kernel.elf
```

## What it does

- Starts at PROM + `0x20` (RED_state, MMUs off), reads the RAM size from
  QEMU's fw_cfg, copies itself to the top 4 MB of RAM, locks a 4 MB TLB
  entry mapping it at `0xf0000000` (where SPARC IEEE 1275 firmware
  lives) and turns both MMUs on. I/O goes through the physical bypass
  ASIs.
- Its own trap table: `clean_window`, 64-bit `spill_0_normal` /
  `fill_0_normal`, and a report for anything else
  (`trap: type ... at tpc ...`).
- Console: the 16550 behind the EBus at PCI I/O `0x3F8`
  (`0x1fe020003f8`). Prints `SquirrelBIOS SPARC`.
- PCI: gives the two Simba (APB) bridges behind the Sabre PBM their bus
  numbers (1 behind `00:01.1`, where the EBus is, 2 behind `00:01.0`) and
  APB address maps, and fills each on-board IMAP register with its
  interrupt group and number (`0x7c0 | ino`), not yet valid. It sizes and
  assigns the BARs of the devices on bus 1 (PCI I/O from `0x4000`, above
  the ISA ports and the NVRAM at `0x2000`; memory from 16 MB) and opens
  Simba A's windows for them. `pci` at the prompt lists them.
- Device tree: `/pci@1fe,0` (`SUNW,sabre`: `reg`, `ranges`,
  `bus-range`, `virtual-dma`, `upa-portid`, its error interrupts), the
  Simba bridges `pci@1` and `pci@1,1`, and under `pci@1,1`:
  `ebus@1/su@14,3f8`, the console, with `interrupts` = INO `0x2b` (ISA
  IRQ 4 on QEMU's EBus); `network@1,1` (hme, INO `0x21`,
  `local-mac-address`), `display@2` (VGA) and `ide@3` (CMD646, INO
  `0x20`), each with `reg` and `assigned-addresses`. PCI nodes carry
  `vendor-id`, `device-id`, `class-code` and `revision-id` from
  configuration space. The root has the NVRAM's ID PROM as `idprom`.
- Reads the kernel address and size, initrd and command line from fw_cfg.
  QEMU loads `-kernel` (an ELF such as Linux, linked at `0x404000`) at its
  link addresses.
- Maps RAM below `CONFIG_IDENTITY_MB` (32 MB) 1:1 in 4 MB pages, so the
  kernel runs at its link address with the MMUs on, and lists every
  mapping in `/virtual-memory` `translations`. Only the firmware's own
  page is locked: the 1:1 entries are global, and an OS that runs
  processes flushes them once it has its own trap table and mappings.
- Without `-kernel`, boots from the IDE disk: an ATA PIO driver on the
  CMD646's assigned BARs (primary channel, polled, LBA28) reads a sparc64
  ELF (such as Linux's `vmlinux`) stored from sector 0, puts its
  `PT_LOAD` segments at their physical addresses and enters it as it
  would a `-kernel`. The slave disk, if any, is the initrd (all of it),
  placed after the kernel. A key already waiting at start keeps the
  firmware at the prompt, where `boot <args>` gives a command line.
- Without a kernel, and after the OS calls `exit`, the `sq>` prompt:
  `info`, `dt [path]`, `pci`, `boot [command line]`, `help`.

## Hand-off

| | |
|---|---|
| Entry | the kernel's entry (fw_cfg), TL 0, `PSTATE` `PRIV` (interrupts off), MMUs on as above, the firmware's trap table in `%tba` |
| `%o4` | the client interface handler |
| `%o0`, `%o1` | initrd address and size (0 without one) |
| `HdrS` | a Linux kernel header in the first 512 bytes gets the initrd too (size at +20; from header version 0x0300 the image at +40, as address - lowest available address + `0x400000`), the way Linux reads it |
| `%sp` | a 16 KB stack in the firmware's page (set your own) |
| Memory | `/memory` `reg` and `available` (page 0, the kernel's 4 MB span, the initrd and the firmware left out) |

## Client interface

Call the handler with `%o0` = an array of 64-bit cells: service name,
number of arguments, number of results, the arguments, room for the
results. It returns 0, or -1 for a service it does not have. `%g1`-`%g7`
come back unchanged (Linux keeps `current` and its thread and per-CPU
pointers in `%g4`-`%g6`, and calls some services without saving them).

| Service | |
|---|---|
| `test` | 0 if the service exists |
| `peer`, `child`, `parent` | `peer(0)` is the root |
| `finddevice` | paths, with or without unit addresses; aliases (`ttya`); `:args` ignored |
| `getproplen`, `getprop`, `nextprop` | every node has a `name` property |
| `instance-to-package`, `package-to-path`, `instance-to-path`, `canon` | |
| `open`, `close`, `read`, `write`, `seek` | the console (`/chosen` `stdin`/`stdout`), the MMU and memory instances; `read` returns -2 when nothing is waiting |
| `claim`, `release` | physical RAM in the 1:1 region (`align` 0: exactly that range) |
| `milliseconds` | from `%tick` |
| `call-method` | MMU: `translate`, `SUNW,itlb-load` / `SUNW,dtlb-load` (virt, data, index: the TLB entry written is the one named); memory: `claim`, `release`. Others fail (catch-result -1) |
| `interpret` | always fails: there is no Forth |
| `SUNW,set-trap-table` | sets `%tba` |
| `exit`, `enter`, `boot` | back to the `sq>` prompt |
| `SUNW,power-off` | halts (QEMU's sun4u has no power-off device wired up) |

## Linux

Linux 6.8 (`sparc64_defconfig`, built from Ubuntu's `linux-source-6.8.0`)
boots to "VFS: Unable to mount root fs" without a root, and with an
initramfs runs `/init`, whose system calls work. It finds the PBM, the
bridges and the EBus, and its `su` driver takes the console with its
interrupt (`ttyS0 at MMIO 0x1fe020003f8 (irq = 4 ...)`), so processes can
write to the console. `pata_cmd64x` drives the IDE controller (QEMU's
DVD-ROM appears as `sr0`), `hme` brings up `eth0` with the ID PROM's MAC
address, and the VGA is the boot VGA device. It boots the same way from
an IDE disk:

    truncate -s %512 vmlinux initrd.cpio      # whole sectors
    qemu-system-sparc64 -M sun4u -nographic -bios sparc64/squirrel-bios.bin \
        -drive file=vmlinux,if=ide,index=0,format=raw \
        -drive file=initrd.cpio,if=ide,index=1,format=raw

or from `-kernel`:

    qemu-system-sparc64 -M sun4u -nographic -bios sparc64/squirrel-bios.bin \
        -kernel vmlinux -initrd initrd.cpio -append console=ttyS0

## Limits (next steps)

- Only the console is described under the EBus: no keyboard, mouse,
  floppy or parallel port. Bus 2 (behind `00:01.0`, where `-device`
  cards go) is not scanned, and the VGA's ROM BAR is left off.
- One CPU is started; others stay in reset.
- No boot from a partitioned disk, a CD (ISO 9660) or the network: the
  IDE disk holds the ELF itself. The IDE driver is polled PIO on the
  primary channel only.
- `claim` with a virtual address outside the 1:1 region and `map` /
  `unmap` are not done.

## Build configuration

`configs/sparc64-sun4u/default.cfg`: `CONFIG_IDENTITY_MB`.

## Test

`make test` runs `tests/sparc64.smoke` (skipped without
`qemu-system-sparc64` or the cross compiler): `tests/sparc64-kernel.c`
checks the entry state and every service above, with 128 MB and 512 MB of
RAM, plus what Linux relies on: globals kept across a call, no locked
1:1 entries, `SUNW,dtlb-load` writing the named entry, the `HdrS`
initrd fields (run with `-initrd`), and the console's interrupt: it
translates `su`'s `reg` through the EBus and PBM `ranges`, reads the
IMAP register for its INO, makes it valid and checks that the UART's
transmitter-empty interrupt reaches the CPU as that vector; and the PCI
devices: their `assigned-addresses` match their BARs in configuration
space, the NIC's MAC is the ID PROM's, and an IDENTIFY PACKET DEVICE sent
through the IDE's assigned I/O BARs comes back from QEMU's DVD-ROM with
an interrupt on the IDE's INO. The same kernel boots from an IDE disk
(with an initrd disk, and with a command line from `boot` after a key
stops the automatic boot), and a disk without a sparc64 ELF is refused;
the prompt and the `default.cfg`
build are checked too.

Original code, written from IEEE Std 1275-1994, its 64-bit SPARC binding
the UltraSPARC-IIi manual and the PCI bus binding, not from another firmware's source.
