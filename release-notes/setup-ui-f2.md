# Setup UI F2 hotkey (Phase G)

Second step of CONTINUANCE Phase G **Setup UI** on SquirrelBIOS: enter
the Warp-look boot menu with **F2** on ConIn, without needing
`opt/org.squirrelworks/setupui`.

- **Hotkey:** after GOP init the firmware prints
  `setup: press F2 for Setup UI` and polls ConIn for a short window
  (~250 ms). Serial ANSI `ESC O Q` (and CSI `ESC [ 12 ~`) map to UEFI
  scan code F2 in `common/efi/textcon.c`. Seeing F2 opens the same menu
  as the fw_cfg path (`setup: F2 — opening Setup UI`).
- **fw_cfg kept:** `-fw_cfg name=opt/org.squirrelworks/setupui,...`
  still forces the menu immediately (no hotkey wait), so
  `x86_64-setupui` is unchanged.
- **Smoke:** `x86_64-setupui-f2` in `tests/smoke.sh` (F2 then
  Down/Up/Enter; no setupui fw_cfg). Existing `x86_64-setupui` still
  covers the fw_cfg gate.
- **Docs:** `x86_64/README.md`, HANDOFF / ROADMAP / TODO Phase G card.

Next: BootOrder edits are in `release-notes/setup-ui-bootorder.md`;
Secure Boot keys / TPM clear in `release-notes/setup-ui-secboot.md`.
Then date-time / NVRAM reset notebooks.
