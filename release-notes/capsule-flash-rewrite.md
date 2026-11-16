# Capsule flash rewrite via NV identity (Phase G M2)

CONTINUANCE / `TO_3.0.0.md` mark **M2** (`2.2.25`): after a verified
signed SQCP apply, persist firmware identity so a second boot sees the
new ESRT `FwVersion`.

- **Spec (public):** UEFI 2.10 §8.5.3 `UpdateCapsule`, §4.6 ESRT.
  Original code; no EDK2/OVMF sources.
- **What “flash rewrite” means under QEMU:** smoke keeps firmware
  pflash **unit 0** read-only (`-drive if=pflash,...,readonly=on`).
  Rewriting the running 1 MB `squirrel-bios.fd` in place is not done.
  Instead apply writes NV variable `SqFwFlashId` (`SQFL` magic +
  target version + body size) through the variable store. With pflash
  **unit 1** that survives reset; without a store the same SetVariable
  path updates ESRT for the current boot only.
- **Caveat line:** on the next boot the firmware prints
  `capsule: flash identity version=0x… (NV stand-in, not real SPI)`
  before publishing ESRT.
- **Apply line:** `capsule: apply ok version=0x… (flash rewrite via NV)`
  (or `(FwVersion only; no NV store)` if SetVariable fails).
- **Smoke:** `x86_64-capsule-flash-1` (apply) and
  `x86_64-capsule-flash-2` (kept identity). Existing
  `x86_64-capsule-signed` / `x86_64-capsule-fwcfg` expect the new
  apply string.
- **Next:** project-key capsule signing / ESRT fwupd-shaped path (M4);
  real SPI rewrite stays out of scope for QEMU smoke.
