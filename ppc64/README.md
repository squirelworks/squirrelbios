# POWER SquirrelBIOS (`ppc64le-powernv`, roadmap E1)

Firmware for QEMU's `powernv9` and `powernv10` machines (bare-metal
POWER9/POWER10, no hypervisor below it), in C with a small assembly
entry. It gives the OS what OPAL-based firmware gives it: a device tree,
an OPAL-style runtime call interface, and parked hardware threads that
the OS starts itself.

```bash
make ppc64            # needs powerpc64le-linux-gnu-gcc (gcc-powerpc64le-linux-gnu)
qemu-system-ppc64 -M powernv9 -smp 4 -nographic -bios ppc64/squirrel-bios.bin -kernel vmlinux
```

## What it does

- QEMU copies the image to physical 0 and starts **every** hardware thread
  at `0x10`, big-endian, hypervisor real mode, `r3` = device tree. The
  first thread to claim the boot word copies the image to `0x30000000`
  (its 4 MB region, `FW_BASE`) and runs C there; the others wait for the
  copy, then park in the relocated image, each on its own stack.
- Console: the 16550 on the LPC bus, found from the device tree
  (`ns16550` under the `ibm,lpc` node) and reached through its MMIO window
  with cache-inhibited loads and stores. Prints `SquirrelBIOS POWER`.
- Reads RAM from the `/memory` nodes, the threads from the CPU nodes'
  `ibm,ppc-interrupt-server#s`, the RTC (`pnpPNP,b00`) and the IPMI BT
  interface to the BMC (`ipmi-bt`).
- Boots the ELF64 that `-kernel` placed at `0x20000000` (either byte order):
  - segments with a load address (`p_paddr`) are copied there and entered
    at `e_entry`;
  - an image with `p_paddr` 0, as Linux's `vmlinux`, runs where it is
    (Linux relocates itself), the way OPAL firmware starts it.
  - A kernel that would overlap the firmware, or run past RAM, is refused.
- Hands over a copy of QEMU's device tree with `/ibm,opal` filled in
  (`compatible = "ibm,opal-v3", "ibm,opal-v2"`, `opal-base-address`,
  `opal-entry-address`, `opal-runtime-size`), an OPAL console node
  (`/ibm,opal/consoles/serial@0`, `ibm,opal-console-raw`),
  `/chosen/stdout-path`, and a memory reservation for the firmware region.
- No kernel (or a refused one): the `sq>` prompt: `info`, `dt [path]`,
  `date`, `boot`, `poweroff`, `reset`, `help`.

## Hand-off

| | |
|---|---|
| Entry | the kernel's entry point, big-endian, hypervisor real mode (`MSR` `SF`, `HV`, `ME` set; `IR`, `DR`, `EE` clear), as OPAL firmware does: a little-endian OS switches itself |
| `r3` | device tree (in the firmware region) |
| `r4` | where an in-place kernel was loaded, else 0 |
| `r5` | 0 (no Open Firmware client interface) |
| `r8`, `r9` | OPAL base and entry (also in `/ibm,opal`) |
| Other threads | parked in the firmware until `OPAL_START_CPU` |

## OPAL-style calls

The OS calls `opal-entry-address` in big-endian real mode with the token in
`r0`, arguments in `r3`-`r10`, the return address in `LR`; `r3` returns the
status. Pointers are physical, data behind them big-endian. Token numbers
and status codes follow the public OPAL API, so an OS written for OPAL
finds the calls where it expects them.

| Token | Call | |
|---|---|---|
| 0 | `OPAL_TEST` | |
| 1, 2 | `OPAL_CONSOLE_WRITE`, `OPAL_CONSOLE_READ` | terminal 0 = the serial console |
| 25, 117 | `OPAL_CONSOLE_WRITE_BUFFER_SPACE`, `OPAL_CONSOLE_FLUSH` | |
| 3, 4 | `OPAL_RTC_READ`, `OPAL_RTC_WRITE` | BCD `YYYYMMDD` / `HHMMSS` |
| 5, 6 | `OPAL_CEC_POWER_DOWN`, `OPAL_CEC_REBOOT` | IPMI chassis control to the BMC (QEMU exits / resets) |
| 10 | `OPAL_POLL_EVENTS` | no events |
| 41, 42 | `OPAL_START_CPU`, `OPAL_QUERY_CPU_STATUS` | started thread gets `r3` = its PIR, big-endian |
| 69 | `OPAL_RETURN_CPU` | the calling thread parks again and can be restarted |
| 70 | `OPAL_REINIT_CPUS` | `HILE_LE` / `HILE_BE`: on POWER8 sets or clears HID0[HILE] on the caller and on every thread started later (POWER9 and later keep the bit in LPCR, which the OS sets) |
| 80 | `OPAL_CHECK_TOKEN` | 1 for the tokens above, else 0 |

Any other token returns `OPAL_PARAMETER` (-1).

## Linux

A stock distribution `vmlinux` (Ubuntu's 6.8 ppc64el kernel was used)
boots on `powernv8` with 1, 2 or 4 cores to the point where it wants a
root file system:

    qemu-system-ppc64 -M powernv8 -smp 2 -m 2G -bios ppc64/squirrel-bios.bin \
        -kernel vmlinux -append console=hvc0

What it takes, besides the calls above:

- POWER8 reaches its LPC bus (serial console, BMC) only through XSCOM:
  the firmware drives the LPC controller's ECCB registers, one byte per
  transaction (a read nobody answers gives `0xff`; the prompt's `info`
  counts transactions that timed out). POWER9 and POWER10 map LPC into
  memory.
- Every thread is handed to the OS with LPCR[LPES0] clear, so external
  interrupts arrive as hypervisor interrupts (HSRR0/1), which is what a
  hypervisor-mode OS handles. QEMU's POWER8 resets with the bit set.
- The boot thread's CPU node comes first in the device tree: Linux makes
  the first node it meets logical CPU 0.
- On POWER9 and later, `ibm,pa-features` advertises radix MMU and GTSE,
  so Linux picks radix.

## Limits (next steps)

- POWER9/POWER10: no interrupt controller (XIVE) calls, so Linux stops at
  "XICS: Cannot find a Presentation Controller"; POWER8 Linux drives its
  XICS itself.
- POWER8 with more than one thread per core (`-smp N,threads=2`): Linux
  starts the sibling threads but then soft-locks; one thread per core
  works.
- No PCIe (PHB) calls: Linux sees no PCI devices, so a root file system
  must come from an initramfs.
- One firmware region at `0x30000000`: RAM must be at least 1 GB (QEMU's
  default).

## Build configuration

`configs/ppc64le-powernv/default.cfg` (see `../configs/README.md`):
`CONFIG_MAX_CPUS`, `CONFIG_KERNEL_BASE`, `CONFIG_KERNEL_MAX`.

## Test

`make test` runs `tests/ppc64.smoke` (skipped without `qemu-system-ppc64`
or the cross compiler): the little-endian test kernel `tests/ppc64-kernel.c`
checks the hand-off, calls every OPAL service, starts, returns and
restarts the other threads and powers off; the prompt, a refused
overlapping kernel and a non-ELF file are checked too. On `powernv8` it
also checks HID0[HILE] and LPCR on every thread and the boot CPU's place
in the tree, and reboots once through the BMC first, so the parked
thread has to come back after a warm reset.

Original code; the OPAL interface numbers come from its public
documentation, not from another firmware's source.
