# Measured boot polish (Phase G M16 / `G-tpm-polish`)

CONTINUANCE / `TO_3.0.0.md` mark **M16** (ladder **2.7.0**): freeze the
PCR measurement policy and TCG2 event-log field layout so Squirrel-OS/2
Control Panel can parse the log without firmware-private headers.

- **Spec (public):** TCG PC Client Platform Firmware Profile (PCR usage,
  crypto-agile `TCG_PCR_EVENT2`), TCG EFI Protocol Specification
  (`EFI_TCG2_PROTOCOL`, Spec ID Event03), TCG Algorithm Registry.
  Original code; no EDK2/OVMF/SeaBIOS.
- **SQTL ActiveBanks:** the last `u32` of `SQUIRREL_TPM_LOG_HANDOFF`
  (formerly `Reserved`) is the `EFI_TCG2_BOOT_HASH_ALG_*` bitmap of
  banks this boot extends. Same offset, same Version **1**; documented
  in `common/handoff.md`.
- **PCR policy v1:** firmware prints
  `tpm: measured-boot policy v1 (TCG2 log + ActiveBanks=…)`. The stand-in
  walks the TCG2 log with only public field sizes and checks the expected
  `(PCR, EventType)` prefix (CRTM + FW blob, Secure Boot variables,
  separators, application measure). Full table: `common/handoff.md`.
- **Smoke:** `x86_64-tpm-log-handoff` (extended) and `x86_64-tpm-polish`
  (needs `swtpm`).
- **Version:** ladder **2.7.0**; `SQ_VERSION` stays **2.3.0** until M14
  cuts the `2.6.0` product banner (same deferral as M15).
- **OS follow-up:** Control Panel “boot security” still belongs to the
  OS/2 lane — this mark only freezes the firmware contract.
- **Next:** **M14** banner when owner is ready; then **M18** rehearsal.
  ~~**M15**~~ SB everyday and ~~**M17**~~ suite matrix are on `main`.
  **M8** skipped; **M10** blocked. Do not take #195 unless asked.

Epic #82. `TO_3.0.0.md` M16.
