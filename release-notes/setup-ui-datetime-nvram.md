# Setup UI date-time / NVRAM reset (Phase G)

Fifth step of CONTINUANCE Phase G **Setup UI** on SquirrelBIOS (lane
**`-2eaf`**, epic #82): Date and time and NVRAM reset notebooks, instead
of printing stubs.

- **Date and time:** Enter opens a notebook on serial + GOP. When the
  CMOS RTC answers `GetTime`, the UI shows `RTC YYYY-MM-DD HH:MM:SS`.
  Left/Right pick a field (Year..Second), Up/Down adjust, Enter applies
  via `SetTime` (MC146818 write through `arch_rtc_set_civil` on
  `x86_64-uefi`; other UEFI arches leave `SetTime` unsupported). A
  read-back checks year/month/day/hour/minute. Esc cancels. If
  `GetTime` fails the notebook reports `RTC unavailable` and Esc backs
  out.
- **NVRAM reset:** Enter opens a confirmation notebook. Under the
  setupui fw_cfg gate only, seeds NV|BS|RT demo vars (`SquirrelDemo`
  and a long name for enum coverage) when absent so clear has something
  to remove. The confirm text states that BootOrder / Boot#### /
  dbt/dbr/MOK-style and other user NV vars are cleared, while Secure
  Boot PK/KEK/db/dbx stay under their own notebook. Confirm deletes
  them with firmware `SetVariable` size 0; Cancel / Esc leave them.
  See also `release-notes/setup-ui-datetime-followup.md`.
- **Smoke:** `x86_64-setupui-datetime` in `tests/smoke.sh` (bump year,
  apply, NVRAM confirm clear, Continue). Existing `x86_64-setupui`,
  `x86_64-setupui-f2`, `x86_64-setupui-bootorder`, and
  `x86_64-setupui-secboot` still cover the shell, F2, BootOrder, and
  Secure Boot / TPM paths.
- **Docs:** `x86_64/README.md`, HANDOFF / ROADMAP / TODO Phase G card.

Setup UI Phase G shell notebooks for CONTINUANCE are complete on this
path; further Setup polish is optional follow-up.
