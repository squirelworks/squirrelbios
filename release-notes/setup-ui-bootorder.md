# Setup UI BootOrder edits (Phase G)

Third step of CONTINUANCE Phase G **Setup UI** on SquirrelBIOS (lane
**`-2eaf`**): the Boot Manager row edits the UEFI `BootOrder` variable for
real, instead of printing a stub.

- **Boot Manager:** Enter opens a Boot Order notebook on serial + GOP.
  When `BootOrder` / `Boot####` are missing (typical under QEMU without a
  prior installer), the firmware seeds `Boot0001` Disk, `Boot0002`
  Network, `Boot0003` USB (minimal `EFI_LOAD_OPTION` with an End device
  path) and `BootOrder = {0001,0002,0003}`.
- **Keys:** Up/Down select an entry; Left/`-` or Right/`+` swap it earlier
  or later in the order; Enter calls `SetVariable` on `BootOrder`
  (NV|BS|RT) and read-backs the value; Esc cancels without writing.
- **Vars early:** `vars_init` / `vars_install` run before the Setup UI so
  edits hit the NVRAM-backed store, not the temporary in-RAM runtime
  stubs.
- **Smoke:** `x86_64-setupui-bootorder` in `tests/smoke.sh` (reorder
  second entry earlier, save, Continue). Existing `x86_64-setupui` and
  `x86_64-setupui-f2` still cover the shell and F2 path.
- **Docs:** `x86_64/README.md`, HANDOFF / ROADMAP / TODO Phase G card.

Next: Secure Boot keys / TPM clear / date-time / NVRAM reset notebooks.
