# VisionFive 2 / HiFive desk notes (Phase G M9 / `2.3.2`)

CONTINUANCE / `TO_3.0.0.md` mark **M9** (`board-vf2-hifive-notes`): desk
boot **procedures** beside the existing `CONFIG=visionfive2` and
`CONFIG=hifive-unmatched` builds. No firmware behaviour change; no new
QEMU machine.

- **VisionFive 2:** [`boards/visionfive2/DESK.md`](../boards/visionfive2/DESK.md)
  — keep vendor SPL (DDR training); place `squirrel-bios.bin` where
  OpenSBI goes (`0x40000000`); UART0 115200; honest “not yet on
  hardware”. CI stays `riscv64-visionfive2-build` (link address only;
  no JH7110 machine).
- **HiFive Unmatched:** [`boards/hifive-unmatched/DESK.md`](../boards/hifive-unmatched/DESK.md)
  — same SPL/OpenSBI split at `0x80000000`; `sifive,uart0`; QEMU
  `sifive_u` remains the CI proxy (`riscv64-hifive-unmatched-build`,
  `riscv64-hifive-unmatched-sifive-u`). Full board / UEFI smoke is desk
  only.
- **Version:** ladder mark **2.3.2** (docs completeness; not a jump to
  `2.4.0`). `SQ_VERSION` stays **2.3.0**.
- **M8:** CI shard remains **skipped** (wall-clock already fine).
- **M10:** RISCV-AIA-MSI still **blocked** on QEMU ≥ 9.1 in CI (8.2 MSI
  storms). Do not force on this image.
- **M3:** Setup UI polish is on `main` (#207); this mark does not redo it.
- **Next:** M11 / M12 desk flashes stay `needs: squirrel`. ~~**M15**~~ /
  ~~**M16**~~ landed; next agent coding is **M14** Phase G product
  banner (prep) or **M17** — not #195, not M10 until CI QEMU moves.
