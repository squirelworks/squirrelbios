# Setup UI date-time / NVRAM follow-ups (Phase G)

Non-blocking notes from PR #185 merge (lane **`-2eaf`**, epic #82):

- **CMOS 12/24h:** `arch_rtc_set_civil` (and `arch_rtc_seconds`) honour
  Status Register B bit 1. In 12-hour mode the hour is written/read as
  1..12 with bit 7 as PM; QEMU stays in 24-hour so the smoke still uses
  the civil path without flipping the bit.
- **NVRAM enum:** `collect_user_nv` uses a 128-CHAR16 name buffer so
  names longer than 31 chars no longer abort the walk. If a name still
  will not fit, the confirm status reports `partial` instead of silently
  omitting later variables. The setupui fw_cfg seed plants a second
  long-named demo var so the grown buffer is covered by
  `x86_64-setupui-datetime`.
- **Confirm copy:** the NVRAM reset notebook states that BootOrder /
  Boot#### / dbt/dbr/MOK-style and other user NV vars are cleared, and
  that PK/KEK/db/dbx are kept (Secure Boot notebook).

Smoke: existing `x86_64-setupui-datetime` (expect 2 demo vars + warn
lines). Other setupui smokes unchanged.
