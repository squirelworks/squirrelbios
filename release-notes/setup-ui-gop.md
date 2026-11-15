# Setup UI on GOP (Phase G, first step)

A sizeable first step toward CONTINUANCE Phase G **Setup UI** on
SquirrelBIOS: a graphical setup / boot menu shell on GOP in a Warp
look, with the same menu mirrored on the serial console.

- **Module:** `x86_64/core/setupui.c` plus `font8x16.h` (same original
  8×16 glyphs as `x86/drivers/vga/mkfont.py`). Title bar, teal desktop,
  grey panel, blue selection highlight; list of Continue boot and stub
  rows (Boot Manager, Secure Boot keys, TPM clear, date/time, NVRAM
  reset).
- **Input:** Up / Down / Enter / Esc through the existing Simple Text
  Input path (serial ANSI). Esc or Continue leaves the menu and the
  normal boot path runs.
- **Gate:** only when QEMU provides
  `-fw_cfg name=opt/org.squirrelworks/setupui,...`, so every other
  smoke stays unchanged.
- **Smoke:** `x86_64-setupui` in `tests/smoke.sh` (navigates with
  Down/Up/Enter, expects `setup: selected Continue boot`).
- **Docs:** `x86_64/README.md`, HANDOFF / ROADMAP / TODO Phase G card.

Follow-up: F2 without fw_cfg is in `release-notes/setup-ui-f2.md`;
BootOrder edits in `release-notes/setup-ui-bootorder.md`.
Next: Secure Boot / TPM / NVRAM notebooks.
