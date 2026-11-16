# Signed capsule apply / PersistAcrossReset staging (Phase G)

CONTINUANCE Phase G follow-up after UpdateCapsule + ESRT (#189): verify
a project-signed system-firmware capsule and stage or apply it under
QEMU without rewriting the flash image.

- **Spec (public):** UEFI 2.10 §8.5.3 `UpdateCapsule`, §4.6 ESRT,
  §8.2 detached PKCS#7 / `WIN_CERTIFICATE_UEFI_GUID`. Original code; no
  EDK2/OVMF sources.
- **Layout:** `EFI_CAPSULE_HEADER` + `SQUIRREL_FW_CAPSULE_HDR` (`SQCP`) +
  body + `WIN_CERTIFICATE_UEFI_GUID` PKCS#7 detached over SHA-256 of
  bytes before the certificate. Header-only capsules remain unsigned.
- **Trust:** fw_cfg `opt/org.squirrelworks/capsule-db` ESL
  (`x86_64/secure/capsule_auth.c`).
- **Apply:** ESRT `LastAttempt*` + `FwVersion` + NV `SqFwFlashId`
  (`capsule: apply ok … (flash rewrite via NV)`). Auth failure:
  `AUTH_ERROR` / `EFI_SECURITY_VIOLATION`. See
  `capsule-flash-rewrite.md` for the QEMU “not real SPI” caveat.
- **PersistAcrossReset:** stages NV `SqCapsuleStaging`; fw_cfg
  `opt/org.squirrelworks/capsule` applies pending at init.
- **Deferred:** SGL-only multi-block gather (SGL-only UpdateCapsule is
  refused — see `capsule-signed-followup.md`); real SPI rewrite.
- **Smoke:** `x86_64-capsule-signed`, `x86_64-capsule-fwcfg`,
  `x86_64-capsule-flash-1`, `x86_64-capsule-flash-2`.
