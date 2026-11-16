# Desk boot notes — VisionFive 2 (JH7110)

Procedure for a real-board check of
`make PLATFORM=riscv64-uefi CONFIG=visionfive2`. Hardware writes and
desk observations stay with the owner (`needs: squirrel`); this note is
the procedure those checks follow. It is never a merge blocker.

Read [`README.md`](README.md) for the research map (harts, UART, gaps).
Upstream QEMU has **no** JH7110 / VisionFive 2 machine, so `make test`
only build-checks the link address (`riscv64-visionfive2-build`).

## What you need

- A VisionFive 2 (or compatible JH7110 board) with known-good power and
  LPDDR4 already trained by the **vendor U-Boot SPL** (we do not replace
  DDR training).
- USB serial on the board UART0 header: **115200 8N1**, no flow control.
- A host that can rebuild the image and edit the vendor FIT / flash or
  SD layout the SPL already uses for OpenSBI.
- Image:
  `build/riscv64-uefi/visionfive2/squirrel-bios.bin`
  (`CONFIG_FW_BASE=0x40000000`).

## Expected role of our image

Keep the vendor SPL. Put **our** binary where that SPL loads OpenSBI
(`fw_dynamic` / OpenSBI `FW_TEXT_START` at `0x40000000` on this board).
Entry contract matches QEMU / `_start`: `a0` = hart id, `a1` = device
tree, `a2` = `fw_dynamic_info`. The UEFI core sits 2 MB after the SBI in
the same image layout as `riscv64-uefi`.

Do **not** flash a bare image in place of the SPL. Without the SPL there
is no DDR training.

## Desk steps (outline)

1. Build on the host:

   ```sh
   make PLATFORM=riscv64-uefi CONFIG=visionfive2
   ```

2. Confirm the SBI link address (same check as CI):

   ```sh
   riscv64-linux-gnu-readelf -h \
     build/riscv64-uefi/visionfive2/obj/sbi.elf | grep Entry
   # expect Entry point address: 0x40000000
   ```

3. Get a console first with the **vendor** OpenSBI + U-Boot still in
   place. Confirm UART0 at 115200 before replacing anything.

4. Replace only the OpenSBI payload in the vendor FIT (or the flash /
   SD slot the SPL uses for OpenSBI) with `squirrel-bios.bin`. Keep the
   SPL and the board device tree the SPL already passes. Exact FIT
   packaging is board-/vendor-specific; use the board’s published boot
   docs, not a third-party bootloader as a code source.

5. Boot-mode switches: choose the medium the SPL is already known to
   load from (QSPI / SD / eMMC per the VisionFive 2 quick-start). Do not
   invent a new first-stage path in this mark.

6. On success you should see early SBI / UART lines on UART0, then (once
   a U74 is the boot hart and monitor-hart park is in place) hand-off
   into the UEFI core. Today’s known gaps that may still halt or limit
   the run are listed in [`README.md`](README.md): monitor hart 0 must
   stay in M-mode, timeouts assume 10 MHz, SRST/power-off still use the
   QEMU finisher path, and DesignWare MSHC / JH7110 PCIe are not driven
   yet.

## Desk status

**Not yet run on hardware.** When a desk boot is done, append a short
dated note here (serial excerpt: banner + first `dt:` / `uart:` lines,
boot medium, FIT slot replaced). Until then this file is procedure only.

## Related CI

| Check | What it proves |
|-------|----------------|
| `riscv64-visionfive2-build` | Image links at `0x40000000` |
| *(none)* | No QEMU JH7110 boot case — desk only |
