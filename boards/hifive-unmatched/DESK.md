# Desk boot notes — HiFive Unmatched (FU740)

Procedure for a real-board check of
`make PLATFORM=riscv64-uefi CONFIG=hifive-unmatched`. Hardware writes and
desk observations stay with the owner (`needs: squirrel`); this note is
the procedure those checks follow. It is never a merge blocker.

Read [`README.md`](README.md) for the research map (harts, `sifive,uart0`,
gaps). Upstream QEMU has **no** Unmatched machine. `make test` build-checks
the link address and boots the same image on QEMU `sifive_u` far enough
for the SBI banner + UART probe (FU540-like; same UART IP/base).

## What you need

- A HiFive Unmatched (FU740) with known-good power and DDR already
  trained by the **vendor U-Boot SPL**.
- USB serial on UART0: **115200 8N1**, no flow control
  (`CONFIG_UART_BASE=0x10010000`, `sifive,uart0`).
- A host that can rebuild the image and edit the vendor FIT / SD or SPI
  layout the SPL uses for OpenSBI.
- Image:
  `build/riscv64-uefi/hifive-unmatched/squirrel-bios.bin`
  (`CONFIG_FW_BASE=0x80000000`).

## Expected role of our image

Keep the vendor SPL. Put **our** binary where that SPL loads OpenSBI
(RAM / `FW_TEXT_START` at `0x80000000` on this board). Entry contract
matches QEMU / `_start`: `a0` = hart id, `a1` = device tree,
`a2` = `fw_dynamic_info`.

Do **not** replace the SPL. Without it there is no DDR training.

## QEMU stand-in (already in CI)

Before a desk write, the putc path is exercised on QEMU `sifive_u`:

```sh
make PLATFORM=riscv64-uefi CONFIG=hifive-unmatched
qemu-system-riscv64 -M sifive_u -smp 5 -m 2G -nographic \
  -bios build/riscv64-uefi/hifive-unmatched/squirrel-bios.bin
```

Expect `SquirrelBIOS RISC-V` and `dt: UART at 0x0000000010010000`.
That is **not** a full Unmatched UEFI smoke: hart 0 on `sifive_u` is an
E51-class monitor core without S-mode; board reset/power-off and the
real FU740 tree stay desk-only.

## Desk steps (outline)

1. Build on the host:

   ```sh
   make PLATFORM=riscv64-uefi CONFIG=hifive-unmatched
   ```

2. Confirm the SBI link address (same check as CI):

   ```sh
   riscv64-linux-gnu-readelf -h \
     build/riscv64-uefi/hifive-unmatched/obj/sbi.elf | grep Entry
   # expect Entry point address: 0x80000000
   ```

3. Get a console first with the **vendor** OpenSBI + U-Boot still in
   place. Confirm UART0 before replacing anything.

4. Replace only the OpenSBI payload in the vendor FIT (or the SD / SPI
   slot the SPL uses for OpenSBI) with `squirrel-bios.bin`. Keep the SPL
   and the board device tree. Exact packaging is board-/vendor-specific;
   use SiFive’s published Unmatched getting-started docs, not a
   third-party bootloader as a code source.

5. On success you should see the SBI banner and
   `dt: UART at 0x0000000010010000` (or the board tree’s UART base).
   Known gaps that may still limit the run are in
   [`README.md`](README.md): monitor-hart park for hart 0, 1 MHz
   `timebase-frequency` vs 10 MHz timeout assumptions, DA9063
   reset/power-off, SD-over-SPI, and DesignWare PCIe for NVMe.

## Desk status

**Not yet run on hardware.** When a desk boot is done, append a short
dated note here (serial excerpt, boot medium, FIT slot replaced). Until
then this file is procedure only; CI covers build + `sifive_u` proxy.

## Related CI

| Check | What it proves |
|-------|----------------|
| `riscv64-hifive-unmatched-build` | Image links at `0x80000000` |
| `riscv64-hifive-unmatched-sifive-u` | SBI banner + UART probe on QEMU `sifive_u` |
| *(none)* | No QEMU Unmatched machine — full board / UEFI smoke is desk only |
