# Suite compatibility matrix (Phase G M17 / `compat-matrix`)

CONTINUANCE / `TO_3.0.0.md` mark **M17** (ladder **2.8.0**): freeze the
**BIOS ↔ OS/2** compatibility matrix so every product release note cites
the same columns — BIOS tag, OS/2 tag (min), boot path (legacy BIOS /
CSM / UEFI), QEMU machine, and the check / script that proves the row.

- **Canonical table:** the suite matrix below is the source of truth.
  Paste or link it from every `release-notes/v*.md` from **v2.2.0**
  onward (host check `compat-matrix` refuses notes that omit a
  Compatibility section).
- **Host check:** `tools/compat_matrix_lint.py` (smoke `compat-matrix`
  via `tests/compat_matrix_selftest.sh`) parses this table, requires
  the path column to name a boot class, verifies published BIOS tags
  have matching `release-notes/v*.md`, and checks that the pinned
  OS/2 partner in `tests/os-boot.pin` is cited on an `os-boot-sync`
  row. `--self-test` covers negative + positive fixtures. No network.
- **Version:** M14 product banner (`2.6.0`) is still open for owner
  prep/tag; this mark keeps `SQ_VERSION` at **2.3.0** (do not jump to
  2.8.0 without the earlier banner cuts). Ladder mark **2.8.0** only.
- **Not in this mark:** cutting a `v2.8.0` GitHub release; Soft PATH
  index edits.
- **Next:** **M14** banner when owner is ready; ~~**M15**~~ /
  ~~**M16**~~ / ~~**M18**~~ on `main`. **M8** skipped; **M10**
  blocked. Skip #195 unless asked.

Epic #82. `TO_3.0.0.md` M17.

## Suite matrix

Fill real SHAs at publish time; keep the columns stable. Rows marked
*(mark)* or *(planned)* are ladder notes without a tagged BIOS release
asset set yet — the host check does not require `release-notes/v*.md`
for those BIOS cells.

| BIOS tag | OS/2 tag (min) | Path | QEMU machine | Check / script |
|----------|----------------|------|--------------|----------------|
| v2.1.1 | v2.1.0 / tip | legacy BIOS, CSM, UEFI | `pc` / `q35` | `tests/os-boot.sh`, OS `uefi-squirrelbios` |
| v2.2.0 | OS/2 tip at cut | legacy BIOS, CSM, UEFI (+ capsule/ESRT) | `q35` (+ swtpm for TPM) | `x86_64-capsule`, Setup UI cases |
| v2.3.0 | OS/2 event-log consumer *(when ready)* | UEFI + TPM | `q35` + swtpm | `x86_64-tpm-log-handoff` |
| v2.3.1 *(mark; SQ_VERSION still 2.3.0)* | **OS/2 `v2.3.0`** (pinned) | legacy BIOS / CSM `os-boot` | `pc` / `q35` | `os-boot-sync`, `tests/os-boot.sh` |
| v2.6.1 *(mark; SQ_VERSION still 2.3.0)* | OS/2 loader stand-in | UEFI SB + TPM | `q35` + swtpm | `x86_64-sb-os2` |
| v2.9.0 *(planned)* | OS/2 2.9.x | UEFI SB + TPM rehearsal | `q35` + swtpm | `x86_64-m3-rehearsal` |
| **v3.0.0** *(planned)* | **OS/2 3.0.0** | verified chain (legacy BIOS / CSM / UEFI) | `q35` + swtpm | aggregate M20 checks |

Soft does not own rows; it may **index** them from squirrelsoft
`docs/CONTINUANCE.md` / `docs/PATH_TO_3.0.0.md`.
