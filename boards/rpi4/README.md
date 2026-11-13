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
| RAM | from `0`, 1-8 GB; the first ~1 GB (or ~3 GB) is the "low" range the DMA engines reach | `virt`: RAM from 1 GB |
| Peripherals ("low peripheral" mode, the default) | `0xFE000000`-`0xFF7FFFFF` | Device memory below 1 GB: matches |
| UART0 (PL011) | `0xFE201000`, clock 48 MHz | `arm,pl011` from the tree: works as is |
| Mini UART (UART1) | `0xFE215040`; the default console on the GPIO header unless `dtoverlay=disable-bt` gives it the PL011 | not supported |
| GIC-400 (GICv2) | distributor `0xFF841000`, CPU interface `0xFF842000` | `arm,gic-400` is already matched |
| Generic timer | 54 MHz (`CNTFRQ`, set by the stub) | read from `CNTFRQ`: works |
| RTC | none | `GetTime` would fail: needs a fallback (e.g. an epoch in a variable) |
| Power off / reset | the PM watchdog block (`brcm,bcm2835-pm`); no PSCI with the stock stub | PSCI only |
| SD card | EMMC2 (SDHCI) | none |
| USB | VL805 xHCI on PCIe (its firmware loaded through the VideoCore mailbox) | none |
| PCIe | Broadcom STB host bridge, not ECAM | `pci-host-ecam-generic` only |

## Gaps, file by file

- `arm/uefi/start.S` copies the image from flash at `0` to RAM at
  `0x40100000`. On the Pi the image is already in RAM at `0x80000`, so it
  needs an entry path that runs in place (or moves itself) and a link
  address the build chooses (`FW_BASE` is a link-time symbol already).
  `x0` carries the device tree, as here; `ARCH_DTB_ADDR` must become the
  value `x0` had instead of a constant.
- `arm/uefi/arch.c` builds an identity map with RAM assumed between 1 GB
  and 256 GB and Device memory below 1 GB. On the Pi, RAM starts at 0 and
  the peripherals sit just under 4 GB, so the map must come from the
  device tree's `/memory` (Normal) with everything else Device.
- Secondary cores: with no PSCI, Linux uses the spin table the stub left.
  The firmware must keep that region reserved (the stub's page at `0`)
  and must not install a PSCI method in its ACPI tables. For SBBR/ACPI
  (and for the `PSCI` the FADT claims), a PSCI provider at EL3 is needed:
  either our own small EL3 stub (`armstub=`) implementing `CPU_ON`,
  `SYSTEM_OFF` and `SYSTEM_RESET` over the spin table and the PM
  watchdog, or a third-party one. The first is original work worth doing
  if this track continues.
- `ResetSystem`: with PSCI absent, reset through the PM watchdog
  (`PM_RSTC`/`PM_WDOG`, *verify* the bit layout in the BCM2835 peripherals
  manual, which the BCM2711 keeps).
- Boot media: an SDHCI driver (EMMC2) under `common/efi` would give Block
  I/O; `disk.c`/`fat.c` already find `\EFI\BOOT\BOOTAA64.EFI` on it. USB
  needs the Broadcom PCIe bridge, the VL805 firmware load and xHCI: much
  more work (D4's shared xHCI would help).
- ACPI: `arm/uefi/acpi.c` builds from the device tree, but it assumes
  PSCI, a PL011 and an ECAM host; on the Pi, ACPI would be DT-only until
  PSCI exists, which is what most Pi distributions use anyway.
- Console: the default `config.txt` gives the PL011 to Bluetooth;
  `dtoverlay=disable-bt` moves it to GPIO 14/15, which the firmware
  already drives from the tree.

## Pi 5 (BCM2712)

The boot flow is similar but the EEPROM boot loader loads the image
directly and the firmware includes a TF-A BL31, so PSCI is there and
the cores start through it. The debug UART is a PL011 on the dedicated
3-pin connector, at `0x107D001000` *(verify)*; the GIC-400 sits near
`0x107FFF9000` *(verify)*; most I/O (GPIO UARTs, USB, Ethernet) is on the
RP1 chip behind PCIe, so SD (on the SoC) is the realistic first boot
medium. Peripherals above 4 GB mean the identity map must cover the
whole 40-bit range the tree uses, which the current 1 TB map does.

## A first milestone

`kernel=squirrel-bios.fd` on a Pi 4 with `dtoverlay=disable-bt`, `enable_uart=1`:
the banner, the memory map from the tree and the GIC on the PL011, then
"nothing to boot". That needs only the in-place entry, the map from
`/memory`, and a reset fallback. SDHCI and an EL3 PSCI stub come after.

## Reading

- Raspberry Pi documentation: "Raspberry Pi hardware", "config.txt"
  (`arm_64bit`, `kernel`, `armstub`, `kernel_address`, `enable_uart`)
- BCM2711 ARM Peripherals (public PDF): the address map, PL011, GIC-400
- BCM2835 ARM Peripherals: the PM and watchdog registers
- Arm Base Boot Requirements (SBBR, BBR) and the PSCI specification
