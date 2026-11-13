# SiFive HiFive Unmatched (FU740)

Research for ROADMAP D3's boards line. The FU740 is close to the
VisionFive 2 in shape ([`../visionfive2/`](../visionfive2/README.md)
lists the shared gaps), with these differences.

## How it boots

The boot ROM loads a U-Boot SPL from SD (or SPI flash), which trains the
DDR4 and loads a FIT with OpenSBI and U-Boot proper. As on the
VisionFive 2, our image would go where OpenSBI goes and be entered in
M-mode with `a0`/`a1`/`a2` as on QEMU.

## Differences that matter to us

From the FU740 device tree and manual (*verify* against the board's
own tree):

| What | FU740 | Ours today |
|---|---|---|
| Harts | hart 0 = S7 monitor core (no S-mode); harts 1-4 = U74 | every hart may be an S-mode target |
| RAM | from `0x80000000`, 16 GB | matches the 2 MB firmware region at `0x80000000`, if the SPL loads us there |
| UART0 | `0x10010000`, `sifive,uart0`: its own register layout (txdata, rxdata, txctrl, rxctrl, div), not a 16550 | `ns16550a` only |
| CLINT | `0x02000000` | taken from the tree |
| Timer | `timebase-frequency` 1 MHz | timeouts assume 10 MHz |
| Power off / reset | the DA9063 PMIC over I2C; no test finisher | the QEMU test finisher |
| Storage | SD over SPI (`mmc-spi-slot`), NVMe on M.2 over PCIe (FU740's DesignWare PCIe) | virtio-blk only |

## Gaps beyond the VisionFive 2 list

- **A second UART kind.** `sifive,uart0` needs its own `putc`/`getc`
  (write `txdata` when bit 31, "full", is clear; read `rxdata` while bit
  31, "empty", is clear; enable with `txctrl`/`rxctrl`; baud from `div`
  and the bus clock). `dt_devices` would record which kind it found and
  `putc` would branch on it; the UEFI core's `arch.c` likewise.
- **SD over SPI.** The boot SD sits on an SPI controller (`sifive,spi0`)
  in MMC-over-SPI mode: a small SPI driver plus the SD SPI-mode command
  set would give Block I/O for `\EFI\BOOT\BOOTRISCV64.EFI`. NVMe needs
  the PCIe host (DesignWare, not ECAM-generic) first.

QEMU's `sifive_u` machine models an FU540 (the predecessor: same UART
kind, same CLINT, hart 0 = E51 monitor core), so the monitor-hart
handling and the `sifive,uart0` driver can be developed and tested in
QEMU before any board is involved: `qemu-system-riscv64 -M sifive_u
-smp 5 -bios squirrel-bios.bin`. That would make a good first
`make test` case for this track.

## Reading

- SiFive FU740-C000 Manual (public): memory map, UART, SPI, CLINT
- HiFive Unmatched Getting Started Guide
- QEMU documentation for `sifive_u`
