# Setup UI Secure Boot keys / TPM clear (Phase G)

Fourth step of CONTINUANCE Phase G **Setup UI** on SquirrelBIOS (lane
**`-2eaf`**): Secure Boot key view/clear and TPM clear confirmation
notebooks, instead of printing stubs.

- **Secure Boot keys:** Enter opens a notebook on serial + GOP that shows
  mode (setup / user from PK presence) and PK / KEK / db / dbx size.
  When none are enrolled and the setupui fw_cfg gate is set, the firmware
  seeds four 1-byte volatile (BS|RT) demo stubs so the view has something
  to show; a partial seed failure scrubs what was written, and stubs are
  scrubbed on clear or when leaving so `secure_init` still sees setup
  mode. **Clear all keys** asks for confirmation, then deletes the four
  variables (firmware `SetVariable` size 0 before auth hooks run). Setup
  UI runs before `secure_init`, so clears apply to this boot's verify
  path (not only the next boot).
- **TPM clear:** Enter opens a confirmation notebook. With no TPM
  (`tpm: none`) Confirm prints `setup: TPM clear skipped (no TPM)`.
  When a TPM is present the confirmation is real but the command is a
  stub (`setup: TPM clear confirmed (stub - not sent)`) so mid-boot
  measured PCRs are not disturbed under QEMU smoke.
- **Smoke:** `x86_64-setupui-secboot` in `tests/smoke.sh` (seed, clear
  four keys, TPM clear with no device, Continue). Existing
  `x86_64-setupui`, `x86_64-setupui-f2`, and `x86_64-setupui-bootorder`
  still cover the shell, F2, and BootOrder paths.
- **Docs:** `x86_64/README.md`, HANDOFF / ROADMAP / TODO Phase G card.

Next: date-time / NVRAM reset are in `release-notes/setup-ui-datetime-nvram.md`.
