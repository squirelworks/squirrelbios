# Capsule project key + ESRT fwupd-shaped path (Phase G M4)

CONTINUANCE Phase G product mark **`2.2.50`** (intentional mid-minor): a
dedicated in-tree **test** project key for SQCP capsules, a signing
recipe, and a host ESRT stand-in shaped like fwupd-class tooling under
QEMU.

- **Spec (public):** UEFI 2.10 §8.5.3 `UpdateCapsule`, §4.6 ESRT,
  §8.2 detached PKCS#7 / `WIN_CERTIFICATE_UEFI_GUID`. Original code; no
  EDK2/OVMF/OpenSSL.
- **Project key:** RSA-2048 derived from seed `0x5153_C001` (CN
  `SquirrelBIOS capsule project test key`). Distinct from Secure Boot
  test key A (`0x5153_0001`). Private material is never committed; smoke
  regenerates `capsule-project-pub.der` and `capsule-project.esl`.
- **Recipe:** `python3 tests/mkcapsule.py --print-recipe` and
  `python3 tests/mkcapsule.py --sign-project OUT.cap`.
- **Trust:** fw_cfg `opt/org.squirrelworks/capsule-db` =
  `capsule-project.esl` (not `sb-db-x509.esl`).
- **ESRT (fwupd-shaped):** one `SYSTEMFIRMWARE` resource, version 1,
  `FwClass` = `SQUIRREL_SYSTEM_FIRMWARE_GUID`,
  `CapsuleFlags` includes `PERSIST_ACROSS_RESET`. A verified apply
  bumps ESRT `FwVersion` + `LastAttempt*` this boot; M2 also persists
  `SqFwFlashId` (NV stand-in — not real SPI). Guest dumps
  `APP esrt-list` / `APP esrt-entry0`; host `tools/esrt_standin.py`
  lists/updates from the serial log.
- **Smoke:** `x86_64-capsule-key`, `x86_64-capsule-key-esrt`,
  `capsule-project-key-host`.
- **Next:** optional Setup polish (M3) if a concrete gap remains;
  measured TPM event-log hand-off (M5). M2 flash rewrite is on `main`.
