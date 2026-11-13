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
- Reads the kernel address and size, initrd and command line from fw_cfg.
  QEMU loads `-kernel` (an ELF such as Linux, linked at `0x404000`) at its
  link addresses.
- Maps RAM below `CONFIG_IDENTITY_MB` (32 MB) 1:1 in locked 4 MB pages, so
  the kernel runs at its link address with the MMUs on, and lists every
  mapping in `/virtual-memory` `translations`.
- Without a kernel, and after the OS calls `exit`, the `sq>` prompt:
  `info`, `dt [path]`, `boot`, `help`.

## Hand-off

| | |
|---|---|
| Entry | the kernel's entry (fw_cfg), TL 0, `PSTATE` `PRIV` (interrupts off), MMUs on as above, the firmware's trap table in `%tba` |
| `%o4` | the client interface handler |
| `%o0`, `%o1` | initrd address and size (0 without one) |
| `%sp` | a 16 KB stack in the firmware's page (set your own) |
| Memory | `/memory` `reg` and `available` (page 0, the kernel's 4 MB span, the initrd and the firmware left out) |

## Client interface

Call the handler with `%o0` = an array of 64-bit cells: service name,
number of arguments, number of results, the arguments, room for the
results. It returns 0, or -1 for a service it does not have.

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
| `call-method` | MMU: `translate`, `SUNW,itlb-load`, `SUNW,dtlb-load`; memory: `claim`, `release`. Others fail (catch-result -1) |
| `interpret` | always fails: there is no Forth |
| `SUNW,set-trap-table` | sets `%tba` |
| `exit`, `enter`, `boot` | back to the `sq>` prompt |
| `SUNW,power-off` | halts (QEMU's sun4u has no power-off device wired up) |

## Limits (next steps)

- One CPU is started; others stay in reset.
- No PCI enumeration, disks or network: the kernel comes from `-kernel`.
- `claim` with a virtual address outside the 1:1 region and `map` /
  `unmap` are not done yet, so a stock Linux does not boot yet.

## Build configuration

`configs/sparc64-sun4u/default.cfg`: `CONFIG_IDENTITY_MB`.

## Test

`make test` runs `tests/sparc64.smoke` (skipped without
`qemu-system-sparc64` or the cross compiler): `tests/sparc64-kernel.c`
checks the entry state and every service above, with 128 MB and 512 MB of
RAM; the prompt and the `default.cfg` build are checked too.

Original code, written from IEEE Std 1275-1994, its 64-bit SPARC binding
and the UltraSPARC-IIi manual, not from another firmware's source.
