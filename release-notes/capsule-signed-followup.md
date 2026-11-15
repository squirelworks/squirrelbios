# Signed capsule apply — Sarah #193 follow-ups

Non-blocking notes on the #193 merge, closed here:

1. **Multi PERSIST** — `SqCapsuleStaging` is one NV slot. `UpdateCapsule`
   with `count > 1` and any `CAPSULE_FLAGS_PERSIST_ACROSS_RESET` now returns
   `EFI_UNSUPPORTED` / `INSUFFICIENT_RESOURCES` attempt status instead of
   last-wins.
2. **Staging size** — stage size is capped to `QueryVariableInfo`
   `MaximumVariableSize` (fallback 32 KB), with
   `capsule: staging too large for NV …` and `EFI_OUT_OF_RESOURCES` when
   over. Still no 1 MB into the variable store.
3. **SGL-only** — header array required. SGL-only `UpdateCapsule` returns
   `EFI_UNSUPPORTED` and does **not** mark `LAST_ATTEMPT_STATUS_SUCCESS`
   (no signature bypass). Full SGL gather remains deferred.
4. **Refused staged** — after a failed apply of a staged capsule, staging
   is still cleared, and the firmware prints
   `capsule: cleared refused staged capsule`.

Smoke: `x86_64-capsule-signed` (adds `sgl=ok` / `multi=ok`).
