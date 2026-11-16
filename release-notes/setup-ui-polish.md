# Setup UI polish slice (Phase G M3 / `G-setup-ui-polish`)

Optional Phase G string / notebook polish (lane **`-2eaf`**, epic #82)
after `setup-ui-followups.md`. No new chrome — only concrete label and
empty-order honesty that remained after the earlier notebooks landed.

- **Root menu title:** the GOP panel and serial header for the top-level
  Setup list said `Boot Manager`, which collided with the Boot Manager
  notebook item. They now say `Main Menu` / `--- Main Menu ---`. The
  Boot Manager notebook still uses `--- Boot Order ---`.
- **Empty BootOrder on F2:** without the setupui fw_cfg demo seed,
  opening Boot Manager printed `BootOrder unavailable`. It now prints
  `BootOrder empty (demo seed is fw_cfg-only)` so the F2 / hardware path
  is explicit about why there is nothing to reorder.
- **Version:** ladder mark was historically `2.2.2`; this slice lands
  after measured hand-off. `SQ_VERSION` stays **2.3.0** (string-only UI
  polish; do not renumber history).
- **M8 note:** CI wall-clock for `make test` on `main` is already ~4–5
  minutes, so optional **M8** `ci-shard-smoke` is **skipped** (do not
  shard for fashion). Revisit only if the suite grows enough that the
  gate hurts.
- **Next:** ~~M9~~ / ~~M15~~ / ~~M16~~ landed; **M14** Phase G product
  banner still open for owner; **M10** blocked on QEMU 8.2; then **M17**.

Smoke: extended `x86_64-setupui` / `x86_64-setupui-f2` /
`x86_64-setupui-bootorder` expect `Main Menu`; new
`x86_64-setupui-polish` (F2, empty BootOrder). Epic #82.
`TO_3.0.0.md` M3.
