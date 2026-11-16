# docs_lint allowlist / honesty strengthen (Phase G M7 / `2.3.25`)

CONTINUANCE §3c harden mark **`2.3.25`**: keep ROADMAP / TODO / HANDOFF
honest against the smoke harness, and refuse stale Phase G / version
claims that redefine product **v3.0.0** as Phase E.

- **Allowlist tighten:** `tools/docs_lint_allowlist.txt` stays audited
  empty on a clean tree. Unused allowlist lines are **errors** (not soft
  notes). Optional rules: `missing-check`, `unexpected-check`,
  `stale-version`, `stale-phase-e`.
- **SQ_VERSION honesty:** live claims in ROADMAP / TODO / HANDOFF
  (`SQ_VERSION` stays/is/at **N.N.N**) must match
  `common/efi/include/sq.h`. Historical release-card wording (published /
  REL- / for the tag / …) may still name older banners.
- **Phase E banner:** ROADMAP Phase E must keep the historical “v3.0”
  disclaimer and point at product **v3.0.0** via
  `release-notes/TO_3.0.0.md` (Phase E ≠ product 3.0.0).
- **Negative fixture:** `python3 tools/docs_lint.py --self-test` (also
  `tests/docs_lint_selftest.sh` / host smoke `docs-lint-harden`) builds a
  bad tree (wrong SQ_VERSION + Phase E marketed as v3.0) and a good tree;
  refuses unused allowlist entries.
- **Version:** ladder mark **2.3.25**; `SQ_VERSION` stays **2.3.0** (no
  firmware behaviour change).
- **Next:** ~~M3~~ / ~~M9~~ landed; **M8** CI shard (`2.3.50`) skipped
  while wall-clock is fine; **M10** AIA MSI blocked on QEMU 8.2. (M6
  os-boot sync is on `main`.)
