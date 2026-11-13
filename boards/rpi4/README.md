# Raspberry Pi 4 (BCM2711), with notes on the Pi 5

Research for ROADMAP D2: running the `aarch64-uefi` firmware (`arm/uefi`
plus `common/efi`) on a Raspberry Pi 4 as a UEFI-class boot stage, so a
generic arm64 distribution (Linux's EFI stub, GRUB) boots from it.

## How the Pi 4 boots

1. The VideoCore GPU runs first. Its boot ROM loads the second stage from
   the SPI EEPROM; that finds a boot medium (SD card, USB, network) and
   loads `start4.elf` and `fixup4.dat` from the FAT boot partition.
2. `start4.elf` reads `config.txt`, sets up SDRAM, builds a device tree
   from the `.dtb` on the partition plus overlays and its own edits
   (memory, `/chosen`, MAC addresses, serial numbers), and loads the ARM
   image: `kernel8.img` with `arm_64bit=1`, or the file `kernel=` names.
3. The ARM cores start in AArch64 at EL3 inside a small stub the firmware
   provides (or `armstub=` replaces). The default stub drops to EL2,
   jumps to the image at `0x80000` on core 0 with `x0` = the device tree,
   and holds cores 1-3 in a spin-table loop (`cpu-release-addr` in the
   tree). It does not provide PSCI.

So the firmware would be `kernel8.img` (or `kernel=squirrel-bios.fd`):
loaded into RAM by the GPU, entered at EL2 with the device tree in `x0`,
which is close to what `arm/uefi/start.S` already expects at EL2.

## The board as the firmware sees it

From the BCM2711 device tree (*verify* against the board's own):

| What | Where | Ours today |
|---|---|---|
| RAM | from `0`, 1-8 GB; the first ~1 GB (or ~3 GB) is the "low" range the DMA engines reach | mapped from `/memory`: works |
| Peripherals ("low peripheral" mode, the default) | `0xFE000000`-`0xFF7FFFFF` | Device memory wherever `/memory` has no RAM: works |
| UART0 (PL011) | `0xFE201000`, clock 48 MHz | `arm,pl011` from the tree, set-up kept: works |
| Mini UART (UART1) | `0xFE215040`; the default console on the GPIO header unless `dtoverlay=disable-bt` gives it the PL011 | not supported |
| GIC-400 (GICv2) | distributor `0xFF841000`, CPU interface `0xFF842000` | `arm,gic-400` is already matched |
| Generic timer | 54 MHz (`CNTFRQ`, set by the stub) | read from `CNTFRQ`: works |
| RTC | none | `GetTime` fails: needs a fallback (e.g. an epoch in a variable) |
| Power off / reset | the PM watchdog block (`brcm,bcm2835-pm`); no PSCI with the stock stub | the watchdog when there is no PSCI |
| SD card | EMMC2 (SDHCI) | `sdhci.c` (tested on the Pi 3's SDHCI in QEMU) |
| USB | VL805 xHCI on PCIe (its firmware loaded through the VideoCore mailbox) | none |
| PCIe | Broadcom STB host bridge, not ECAM | `pci-host-ecam-generic` only |

## What works now (the `rpi4` and `rpi3` builds)

`make PLATFORM=aarch64-uefi CONFIG=rpi4` builds
`build/aarch64-uefi/rpi4/squirrel-bios.fd` for the Pi 4 (`rpi3` for the
Pi 3, BCM2837). `make test` runs the `rpi3` build on QEMU's `raspi3b`
machine (QEMU 8 has no `raspi4b`) in the `rpi3-uefi-sd` case, loaded
the way the Pi's firmware loads `kernel8.img`:

- **Entry in place.** Linked at `0x80000` (`CONFIG_FW_BASE`), so
  `start.S` copies nothing; the device tree comes from `x0`, which is
  what the Pi's stub (and QEMU's) passes. The `virt` build still takes it
  from the start of RAM when `x0` is 0.
- **Memory map from the tree.** `arch_mmu_init` reads `/memory` before
  the MMU is on: Normal memory exactly where RAM is, Device elsewhere,
  with 2 MB blocks in the gigabytes that hold both (RAM from 0, the
  peripherals at `0x3F000000` on the Pi 3 and `0xFE000000` on the Pi 4).
  Page 0 (the stub and the spin table) and the tree's `/memreserve/`
  entries stay reserved.
- **Devices through `ranges`.** `reg` is translated from the bus
  addresses (`0x7E...`), so the PL011, the GIC-400 (Pi 4) and the rest
  are found where the ARM sees them. A PL011 the firmware already
  enabled (`enable_uart=1`) keeps its clock and baud rate.
- **Reset and power off without PSCI.** Through the PM watchdog
  (`brcm,bcm2835-pm-wdt`): a full reset, or reset status partition 63
  for "halt" (QEMU powers off on it).
- **SD card.** `common/efi/sdhci.c` with `sdhci_dt.c`: the Pi 3's
  `brcm,bcm2835-sdhci` and the Pi 4's `brcm,bcm2711-emmc2`, 32-bit
  register accesses only, PIO. The ESP's `\EFI\BOOT\BOOTAA64.EFI`
  boots from it.

To try it on a Pi 4 (not yet done on hardware): copy
`squirrel-bios.fd` to the FAT boot partition, and in `config.txt` set
`arm_64bit=1`, `kernel=squirrel-bios.fd`, `enable_uart=1` and
`dtoverlay=disable-bt` (the PL011 on GPIO 14/15). The console is the
PL011 at 115200.

## Still open

- **On hardware:** everything above has run only on QEMU's `raspi3b`.
  The EMMC2 on the Pi 4 needs its base clock: the tree's
  `clock-frequency` if present, else the capabilities, else a 50 MHz
  guess (*verify*; the firmware mailbox can report the real one).
- **PSCI.** Linux uses the stub's spin table (the CPUs' `enable-method`
  stays as the tree says), so SMP works without it, but the ACPI tables
  need PSCI and are skipped (`acpi: no tables`). An EL3 stub of our own
  (`armstub=`) could reuse `arm/uefi/el3.c`'s PSCI over the spin table.
- **RTC.** None: `GetTime` fails (the test app reports `time=bad`).
- **USB.** The VL805 xHCI sits behind the Broadcom PCIe bridge (not
  ECAM) and needs its firmware loaded through the mailbox first.

## Pi 5 (BCM2712)

The boot flow is similar but the EEPROM boot loader loads the image
directly and the firmware includes a TF-A BL31, so PSCI is there and
the cores start through it. The debug UART is a PL011 on the dedicated
3-pin connector, at `0x107D001000` *(verify)*; the GIC-400 sits near
`0x107FFF9000` *(verify)*; most I/O (GPIO UARTs, USB, Ethernet) is on the
RP1 chip behind PCIe, so SD (on the SoC) is the realistic first boot
medium. Peripherals above 4 GB mean the identity map must cover the
whole 40-bit range the tree uses, which the current 1 TB map does.

## Reading

- Raspberry Pi documentation: "Raspberry Pi hardware", "config.txt"
  (`arm_64bit`, `kernel`, `armstub`, `kernel_address`, `enable_uart`)
- BCM2711 ARM Peripherals (public PDF): the address map, PL011, GIC-400
- BCM2835 ARM Peripherals: the PM and watchdog registers
- Arm Base Boot Requirements (SBBR, BBR) and the PSCI specification
