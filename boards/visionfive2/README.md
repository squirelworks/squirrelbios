# StarFive VisionFive 2 (JH7110)

Research for ROADMAP D3's boards line: running the RISC-V SBI
(`riscv/squirrel-bios.S`) and the UEFI core (`riscv64-uefi`) on a
VisionFive 2 in place of the usual OpenSBI + U-Boot proper.

## How it boots

1. The JH7110 boot ROM (on the S7 monitor core and the U74 cores) loads
   the first stage from QSPI flash, SD or eMMC, chosen by the boot-mode
   switches. That first stage is a U-Boot SPL: it trains the LPDDR4 and
   then loads a FIT image holding OpenSBI (as `fw_dynamic`) and U-Boot.
2. OpenSBI runs in M-mode, U-Boot in S-mode on top of it; U-Boot boots
   Linux (EFI or `booti`).

DDR training is the hard, board-specific part and stays with the SPL:
writing our own is out of scope (it is the C5-style "research, never a
blocker" item). The realistic plan is to keep the vendor SPL and put
**our image where OpenSBI goes**: the SPL then enters it in M-mode with
`a0` = hart ID, `a1` = device tree, `a2` = `fw_dynamic_info`, which is
the same contract QEMU uses and `_start` already follows. With the
`riscv64-uefi` layout (SBI, then the UEFI core 2 MB later), the UEFI
core would replace U-Boot proper as well.

## The board as the firmware sees it

From the JH7110 device tree (*verify* against the board's own):

| What | Where | Ours today |
|---|---|---|
| Harts | hart 0 = S7 (RV64IMAC, M and U only: no S-mode, no MMU); harts 1-4 = U74 (RV64GC, Sv39) | every hart may be an S-mode target |
| RAM | from `0x40000000`, 2/4/8 GB | `0x80000000` assumed for the 2 MB firmware region and PMP |
| UART0 | `0x10000000`, `snps,dw-apb-uart`, `reg-shift = 2`, `reg-io-width = 4`, 24 MHz | `ns16550a`, byte registers, 3.6864 MHz divisor |
| CLINT | `0x02000000` (`riscv,clint0`) | taken from the tree |
| PLIC | `0x0C000000` | left to the OS: fine |
| Timer | `timebase-frequency` 4 MHz | the UEFI core reads it from the tree; the SBI's `SRST_WAIT` and `HSM_CHECKIN_WAIT` assume 10 MHz |
| Power off / reset | no `sifive,test` finisher; the PMIC (AXP15060 over I2C) and the SoC's reset controller | the QEMU test finisher only |

## Gaps, file by file

- **The monitor hart.** `_start` treats any hart below `MAX_HARTS` as
  usable. On the JH7110, hart 0 must stay in M-mode for ever: it should
  park without ever being reported to `sbi_hart_start` as startable
  (HSM must return `SBI_ERR_INVALID_PARAM` for it), and the boot hart must
  be a U74 (the SPL's `fw_dynamic_info.boot_hart`, which `_start`
  already honours). Telling them apart: the `/cpus` node's
  `riscv,isa` / `mmu-type`, or `status = "disabled"` on the S7.
- **The firmware's place.** `FW_BASE` (`0x80000000`) and the 2 MB PMP
  region are constants; here RAM starts at `0x40000000`, and the SPL's
  load address decides where we are. `FW_BASE` should come from where
  `_start` finds itself (`auipc`), and `dt_reserve` already writes the
  region it is given into `/reserved-memory`.
- **The UART.** The DesignWare UART is 16550-compatible but with 32-bit
  registers 4 bytes apart. `putc`, `uart_init` and DBCN should read
  `reg-shift` and `reg-io-width` from the tree (the UEFI core's
  `arch.c` too), and take `clock-frequency` for the divisor, or skip
  programming the divisor and keep what the SPL set.
- **Timeouts.** `SRST_WAIT`, `HSM_CHECKIN_WAIT` and `RAS_SHUTDOWN_WAIT`
  are counted in 10 MHz ticks; they should be scaled by the tree's
  `timebase-frequency`.
- **Reset and power off.** SRST needs a board path: a SoC reset through
  the system reset controller and power-off through the PMIC over I2C
  (*verify* both in the JH7110 TRM). Until then SRST should return
  `SBI_ERR_NOT_SUPPORTED` rather than write to a finisher that is not
  there.
- **Extensions.** The U74 has no Sstc, Zicbom or Zicboz: `enter_s`
  already probes `menvcfg` and leaves them off, and `sbi_time` falls back
  to the CLINT. Good as is.
- **The UEFI core.** It finds RAM, the UART and the timebase in the
  tree; it needs the same `reg-shift` UART support, a disk (SDHCI/DW
  MMC for SD and eMMC, or NVMe on the M.2 slot over PCIe) and an RTC
  fallback (`GetTime` without the goldfish RTC).

## A first milestone

Our `riscv64-virt` SBI (with `-kernel`-style payload) as the SPL's
OpenSBI replacement, booting U-Boot proper or a Linux `Image` in S-mode
on a U74 hart, with the console on UART0 at 115200. That needs the
monitor-hart handling, a position-independent `FW_BASE`, the
`reg-shift` UART and scaled timeouts.

## Reading

- JH7110 Technical Reference Manual and Datasheet (StarFive, public)
- VisionFive 2 Single Board Computer Quick Start Guide (boot-mode
  switches, serial pins)
- The board's device tree as Linux sees it
- RISC-V SBI specification (HSM, SRST), Privileged ISA (PMP, `menvcfg`)
