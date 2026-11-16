# Measured TPM event-log hand-off (Phase G M5 / `2.3.0`)

CONTINUANCE Phase G product mark **`2.3.0`**: the TCG2 crypto-agile event
log is a first-class hand-off surface for Squirrel-OS/2 (boot-security
page later on the OS lane).

- **Spec (public):** TCG EFI Protocol Specification (`EFI_TCG2_PROTOCOL`
  GetEventLog, `EFI_TCG2_FINAL_EVENTS_TABLE`), TCG ACPI Specification
  (TPM2 table log area), TCG PC Client Platform Firmware Profile (TCG2
  log format). Original code; no EDK2/OVMF/SeaBIOS.
- **SQTL configuration table:** GUID
  `SQUIRREL_TPM_LOG_HANDOFF_GUID`, structure magic `'SQTL'`
  (`SQUIRREL_TPM_LOG_HANDOFF` in `common/efi/include/efi.h`). Fields name
  physical `LogBase` / `LogSize` / `LogLast`, format TCG2 (= 2), PRESENT
  / TRUNCATED flags, and the final-events table. Installed only when a
  TPM was started; refreshed as the log grows and after GetEventLog.
- **Same log, three views:** SQTL, ACPI TPM2 log-area fields, and
  `GetEventLog` all describe one ACPI-NVS buffer. After
  `ExitBootServices` the SQTL pointer and Spec ID Event03 header remain
  readable (stand-in checks this).
- **Smoke:** `x86_64-tpm-log-handoff` (needs `swtpm`).
- **Version:** `SQ_VERSION` **2.3.0**; capsule test `TargetVersion` moves
  to `0x00020400` so apply smokes still upgrade past the new ESRT base.
- **OS follow-up:** Squirrel-OS/2 Control Panel “boot security” consumer
  and OS regress name are **not** invented here — coordinate with the
  OS/2 lane when ready (`TO_3.0.0.md` compatibility matrix).
- **Next:** optional Setup polish (M3); M6 `os-boot.sh` keep-in-step;
  M16 measured-boot polish later. (M4 project-key is on `main`.)
