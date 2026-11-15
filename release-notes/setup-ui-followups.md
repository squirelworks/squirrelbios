# Setup UI non-blocking follow-ups (Phase G)

Small hardening pass on the Setup UI stack from PRs #179 / #181 / #182 /
#183 (lane **`-2eaf`**):

- **ASCII punctuation:** GOP / serial strings use plain `-` instead of
  UTF-8 em dashes so the 8×16 font and COM1 do not draw garbage glyphs.
- **ConIn failure:** `setup_ui_run` and the notebooks break out (continue
  boot / leave the notebook) when ConIn is missing or `WaitForEvent`
  fails, instead of busy-looping forever.
- **Selection fill clamp:** `panel_w - 16` is clamped so narrow modes do
  not underflow the selection fill width.
- **F2 window:** the 250 ms hotkey wait is skipped when ConIn / WaitForKey
  is absent.
- **BootOrder demo seed:** fake `Boot####` / `BootOrder` are planted only
  under the setupui fw_cfg test gate (not on F2 alone). Enter skips
  `SetVariable` when the order is unchanged.
- **Secure Boot demo stubs:** volatile BS|RT only, fw_cfg-gated; scrub on
  partial seed failure. Clear applies this boot (Setup runs before
  `secure_init`).

Smokes: existing `x86_64-setupui`, `x86_64-setupui-f2`,
`x86_64-setupui-bootorder`, `x86_64-setupui-secboot` (F2 expect string
updated to ASCII `-`).
