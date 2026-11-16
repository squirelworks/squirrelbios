# os-boot.sh keep-in-step (Phase G M6 / `2.3.1`)

CONTINUANCE Phase G product mark **`2.3.1`**: pin `tests/os-boot.sh` to a
documented Squirrel-OS/2 release so BIOS ↔ OS compatibility stays auditable.

- **Pin file:** `tests/os-boot.pin` names `OS2_TAG`, `OS2_TIP`, and the
  `os.img` / `uefi.img` SHA-256 values from that GitHub Release. Current
  pin: **Squirrel-OS/2 `v2.3.0`**
  (`59069e31989828ec5f6973a44f73b31ec04bb035`).
- **Script:** `tests/os-boot.sh` loads the pin, refuses an image whose
  SHA-256 does not match (message names expected tag/tip/SHA vs got),
  supports `--fetch` (download the pinned `os.img` into `build/os-boot/`),
  and `--check-pin` (host self-test of the pin + mismatch text).
  `OS_SKIP_PIN=1` bypasses the hash for a local tip build.
- **Smoke:** `os-boot-sync` in `make test` runs `--check-pin` (no image
  in the tree; no network). Full VGA boot remains optional:
  `make os-boot` / `make os-boot OS_IMG=…` / `tests/os-boot.sh --fetch`.
- **Docs:** HANDOFF and `common/handoff.md` cite the same OS/2 tag.
  Compatibility matrix row for BIOS ↔ OS/2 lives in
  `release-notes/compat-matrix.md` (M17) and is mirrored in
  `TO_3.0.0.md`.
- **Not in this mark:** bumping `SQ_VERSION` (stays **2.3.0** until a
  cut that asks for it); OS `uefi-squirrelbios` regress ownership stays
  on the OS/2 lane.
- **Next:** ~~M3~~ / ~~M7~~ / ~~M17~~ landed; **M8** CI shard skipped
  while wall-clock is fine.
