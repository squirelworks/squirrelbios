# Secure Boot everyday path (Phase G M15 / `G-sb-everyday`)

CONTINUANCE / `TO_3.0.0.md` mark **M15** (ladder **2.6.1**): the everyday
QEMU recipe that boots a **project-signed Squirrel-OS/2 loader stand-in**
with Secure Boot on and a TPM, and refuses an unsigned image.

- **Why:** C4 already verifies Authenticode against fw_cfg db/dbx; C5
  already measures into PCRs. Product v3.0.0 needs one documented path
  that combines them the way an OS/2 UEFI loader will: SB on → signed
  loader starts → unsigned refused → TPM extends stay green.
- **Stand-in:** `tests/x86_64-sb-os2.asm` prints
  `APP os2-loader cap=ok log=ok extend=ok pcr8=ok`, then `TPMLOG` /
  `TPMPCR` for `tests/tcglog.py`. Signed with test key A via
  `tests/mksigned.py --sign` → `build/tests/sb/sb-os2-signed.efi`.
- **Smoke:**
  - `x86_64-sb-os2` — fw_cfg boot of the signed loader with
    `opt/org.squirrelworks/db` + swtpm; tcglog replay against the
    signed image.
  - `x86_64-sb-os2-esp` — same loader as `\EFI\BOOT\BOOTX64.EFI` on a
    GPT ESP (everyday disk path).
  - `x86_64-sb-os2-unsigned` — unsigned stand-in refused
    (`EFI_SECURITY_VIOLATION`); no `APP os2-loader`.
- **Version:** M14 product banner (`2.6.0`) is still open for owner
  prep/tag; this mark keeps `SQ_VERSION` at **2.3.0** (do not jump to
  2.6.1 without the 2.6.0 banner cut).
- **Not in this mark:** Setup UI enroll notebooks (already covered by
  `x86_64-setupui-secboot` / `x86_64-sbe-*`); real OS/2 PE from a
  release tag (M18 rehearsal uses a stand-in); M16 PCR-policy polish
  (separate mark — now landed).
- **Next:** ~~**M16**~~ / ~~**M17**~~ / ~~**M18**~~ landed; **M14**
  Phase G product banner (prep / owner tag) when ready.
  ~~**M9**~~ VisionFive2 / HiFive desk notes are on `main`; **M10**
  stays blocked on QEMU ≥ 9.1; desk **M11–M12** stay
  `needs: squirrel`. Skip #195 unless asked.

Epic #82. `TO_3.0.0.md` M15.
