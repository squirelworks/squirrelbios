# UpdateCapsule + ESRT (Phase G)

CONTINUANCE Phase G capsule-update plumbing on SquirrelBIOS (lane
`-2eaf`): enough for an OS or `fwupd`-style client to find the ESRT,
query capabilities and submit a system-firmware capsule.

- **Spec (public):** UEFI 2.10 §8.5.3 `UpdateCapsule` /
  `QueryCapsuleCapabilities`, §4.6 EFI System Resource Table, §8.5.4
  `OsIndicationsSupported` (file capsule delivery). Original code; no
  EDK2/OVMF/SeaBIOS sources.
- **Module:** `common/efi/capsule.c` (on x86_64 built into the runtime
  pages with `runtime.c`). Types in `common/efi/include/efi.h`.
- **ESRT:** one system-firmware resource (`FwClass` =
  SquirrelBIOS project GUID), `FwVersion` from `SQ_VERSION`, installed
  as a configuration table. Allocation or install failure prints a
  message and `boot halted`.
- **Runtime:** `UpdateCapsule` accepts the project capsule GUID,
  updates ESRT `LastAttempt*`, optional
  `CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE` before ExitBootServices,
  `CAPSULE_FLAGS_PERSIST_ACROSS_RESET` requires a scatter-gather list,
  `CAPSULE_FLAGS_INITIATE_RESET` warm-resets. Does not rewrite the QEMU
  flash image.
- **OsIndicationsSupported:** volatile BS|RT variable with the
  file-capsule and capsule-result bits.
- **Follow-up:** `release-notes/capsule-signed-apply.md`.
- **Smoke:** `x86_64-capsule` in `tests/smoke.sh` (`tests/x86_64-capsule.c`).
- **Docs:** `common/efi/README.md`, `x86_64/README.md`, HANDOFF /
  ROADMAP / TODO Phase G card, `common/handoff.md`.
