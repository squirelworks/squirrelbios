# Pre-3.0 verified-chain rehearsal (Phase G M18 / `G-m3-rehearsal`)

CONTINUANCE / `TO_3.0.0.md` mark **M18** (ladder **2.9.0**): one QEMU
recipe that boots a **project-signed** OS/2 loader stand-in with Secure
Boot on and a TPM, and shows both **measure** and **SQTL event-log
hand-off** lines (including M16 PCR policy v1) on the serial console.
Companion rehearsal for the OS/2 2.9 bridge — **not** a Phase E redo and
**not** a product banner cut.

- **Why:** M15 already proves SB + signed stand-in + TPM extends; M5/M16
  already prove SQTL / ACPI TPM2 / GetEventLog / ActiveBanks / policy
  v1. Product v3.0.0 needs one documented aggregate that runs them under
  the same machine args so remaining gaps are visible in one place.
- **Stand-in:** `tests/x86_64-m3-rehearsal.c` prints
  `APP m3-rehearsal cfg=ok acpi=ok log=ok tcg2=ok extend=ok`, then
  `APP m3-rehearsal ebs=ok` after ExitBootServices. Signed with test
  key A via `tests/mksigned.py --sign` →
  `build/tests/sb/m3-rehearsal-signed.efi`.
- **Smoke:** `x86_64-m3-rehearsal` — `q35` + swtpm +
  `opt/org.squirrelworks/db` + signed stand-in on fw_cfg; expects SB
  verify, TPM present, SQTL hand-off, measured-boot policy v1, and the
  APP lines above.
- **Version:** ladder **2.9.0**; `SQ_VERSION` stays **2.3.0** until M14
  cuts the `2.6.0` product banner (same deferral as M15/M16). Do not
  jump `SQ_VERSION` to 2.9.0 in this mark.
- **Honest gaps (this mark):**
  - **Real OS/2 2.9 PE** is not available yet — project-signed stand-in
    only (same test key as M15).
  - **Signed capsule apply** is not part of this single QEMU recipe
    (covered by `x86_64-capsule-signed` / `x86_64-capsule-key`).
  - **M14** `2.6.0` / **M20** `3.0.0` banners stay owner cuts.
  - ~~**M17**~~ suite matrix landed on `main` (not a hard dep for the
    smoke).
- **Next:** **M14** banner when owner is ready; then **M19** buffer /
  **M20** prep. **M8** skipped; **M10** blocked. Desk **M11–M12** stay
  `needs: squirrel`. Skip #195 unless asked.

Epic #82. `TO_3.0.0.md` M18.
