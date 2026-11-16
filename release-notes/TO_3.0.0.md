# Path to 3.0.0 — SquirrelBIOS walkthrough (lane `-2eaf`)

**Canonical home:** this file in **squirrelbios** (`release-notes/TO_3.0.0.md`).
Soft [`docs/PATH_TO_3.0.0.md`](https://github.com/squirelworks/squirrelsoft/blob/main/docs/PATH_TO_3.0.0.md)
is a **suite index only** — Soft does **not** own the BIOS version ladder.
OS/2’s companion walkthrough is
[`documentation/releases/TO_3.0.0.md`](https://github.com/squirelworks/squirrel-os2/blob/main/documentation/releases/TO_3.0.0.md)
(PR [#205](https://github.com/squirelworks/squirrel-os2/pull/205) / `cursor/release-coord-b55f`).

**Index:** [ROADMAP.md](../ROADMAP.md) (milestones) · [TODO.md](../TODO.md)
(boxes) · [HANDOFF.md](../HANDOFF.md) (live status / suggested order) ·
epic [#82](https://github.com/squirelworks/squirrelbios/issues/82) (Phase G /
product ticks) · hand-off tracker [#165](https://github.com/squirelworks/squirrelbios/issues/165).

**Audience:** the sole BIOS Cursor lane (`-2eaf`) and the owner (tags only).
There is **no** `-b55f` on BIOS; that lane is OS/2 long mode. Written so a
credit-reset resume can start cold without reinventing the plan.

**Supersedes (2026-11-15):** any reading of ROADMAP **Phase E — “v3.0: other
architectures”** as the definition of product **v3.0.0**. Phase E is
**already done** and shipped under **v2.1 / v2.2** content (POWER, SPARC,
m68k, MIPS, LoongArch, s390x). Do **not** redefine 3.0.0 as redoing Phase E.
See [Phase E vs product v3.0.0](#phase-e-vs-product-v300) below.

---

## Phase E vs product v3.0.0

| Label | Meaning | Status |
|-------|---------|--------|
| ROADMAP **Phase E** (historical banner “v3.0: other architectures”) | New CPU families under QEMU | **Done** on `main`; content rides **v2.1.x / v2.2.0** |
| Product **`v3.0.0` tag** | CONTINUANCE **M3** companion to OS/2 **3.0.0** | **Not yet** — this ladder |

**BIOS v3.0.0** = firmware that everyday boots **Squirrel-OS/2 3.0.0** on the
verified chain:

1. **Secure Boot** on (`x86_64-uefi`) with the project’s own keys.
2. **TPM measure** of the loader / OS image into PCRs (TCG2 + event log).
3. **OS hand-off of the event log** so Squirrel-OS/2 can show a boot-security
   page (coordinate the smoke name with the OS lane later).
4. **Signed capsule product path** (verify + apply + flash rewrite as far as
   QEMU allows; ESRT shaped for `fwupd`-class tooling).
5. **Product polish** (Setup UI remainder, docs/CI honesty, board notes).

Suite context: squirrelsoft
[`docs/CONTINUANCE.md`](https://github.com/squirelworks/squirrelsoft/blob/main/docs/CONTINUANCE.md)
§M3 and §4 Phase G.

---

## Version ladder scheme (why these bumps)

SquirrelBIOS tags are `vMAJOR.MINOR.PATCH` (owner only). This walkthrough
adds **fine-grain marks** so credit-reset resumes have named checkpoints.
Mirror of the OS/2 scheme, with BIOS examples:

| Bump | When to use | BIOS example |
|------|-------------|--------------|
| **+0.0.1** | One PR-sized box; tiny acceptance surface | `2.2.1` = signed capsule verify/apply alone |
| **+0.0.25** | Small multi-file slice, or the second half of a pair that should not wait for a full minor | `2.2.25` = flash rewrite closing the capsule apply pair |
| **+0.25.0** | Medium milestone: a ROADMAP subsection closes, banner may move | `2.5.0` = Phase D “full firmware” banner (ARM/RISC-V product completeness under QEMU + desk notes) |
| **+X.Y.0 (new minor/major)** | Product ship matching CONTINUANCE / OS companion | `2.3.0` = measured hand-off spine; `3.0.0` = M3 verified chain + polish |

**Rules agents must keep:**

1. Only the **owner** creates GitHub tags / Releases (`AGENTS.md`). Agents
   prepare `release-notes/vX.Y.Z.md` (or a design note under `release-notes/`),
   bump `SQ_VERSION` in `common/efi/include/sq.h` when the cut asks for it,
   and leave a **draft** ready for Publish — never a stub empty release.
   Preferred path: `Actions → Draft release` (`.github/workflows/release-draft.yml`)
   with `version` + tip SHA. That workflow runs `make` / `make test`, attaches
   the six images + `SHA256SUMS.txt`, and sets the release body from
   `release-notes/vX.Y.Z.md` via `--notes-file` (full markdown, not a
   download link). If workflow dispatch is denied to the agent, rebuild
   the same file set locally and `gh release upload` onto a draft; still
   never Publish. Do **not** hand the owner a `releases/new?…` stub that
   omits the notes body or the assets.
2. **Original code only.** Specs (UEFI, ACPI, TCG, PI) are fine; cite them.
   Never vendor SeaBIOS, EDK2/OVMF, OpenSBI, U-Boot, coreboot, OpenBIOS, SLOF,
   or Linux firmware sources.
3. Every behaviour change: a `make test` case (or named host `make fuzz-*`),
   `make docs-lint` honesty, `common/handoff.md` / platform README when
   visible, boxes in `TODO.md` / `ROADMAP.md`, and a tick comment on
   epic [#82](https://github.com/squirelworks/squirrelbios/issues/82) for
   Phase G product work.
4. **No stacking.** Every PR bases on `main`. Never stack on another feature
   branch. Prefer one sizeable PR per mark.
5. Do **not** invent parallel version lines. Prefer the next free mark below;
   if a slice lands out of order, insert a `+0.0.1` and note the depends-on.
6. Lane on every checkpoint: **`-2eaf`** or **`needs: squirrel`**. Soft PATH
   is suite index only.
7. Real hardware flashes are post-merge notes, never merge blockers — but
   they still get marks here so the owner can resume them.

---

## Phase 0 — Finish open PRs, then cut v2.2.0 (now)

### Gate: open work that must settle before / with the tip cut

| Item | Lane | Branch / PR | Purpose | Merge gate |
|------|------|-------------|---------|------------|
| [#190](https://github.com/squirelworks/squirrelbios/pull/190) | `-2eaf` | `cursor/bios-rel-2.2.0-refresh-2eaf` | Fold UpdateCapsule/ESRT (#189) into `release-notes/v2.2.0.md`; HANDOFF/ROADMAP/TODO honesty | **MERGED** on `main` |
| Signed capsule worker *(if in flight)* | `-2eaf` | check `gh pr list` / local WIP | Verify + apply / PersistAcrossReset staging | Prefer **land after M0** as M1 unless already CI-green and owner wants it in the tip; do **not** hold M0 forever |

Capsule/ESRT plumbing is **already on `main`** via
[#189](https://github.com/squirelworks/squirrelbios/pull/189) (`x86_64-capsule`).
Notes refresh **#190** is on `main`. Soft PATH’s fold-into-2.2.0 advice is satisfied — owner can tag **v2.2.0** (no separate v2.2.1 for #189 alone).

### M0 — `v2.2.0` tip cut  ·  **done (owner published)**  ·  depends: #190 on `main` *(done)*

| Field | Value |
|-------|-------|
| **Version** | `2.2.0` (`SQ_VERSION` already `2.2.0` on `main`) |
| **Tag target** | `0051c40` on `main` — tip includes #189 capsule/ESRT and #190 notes |
| **Notes** | [`v2.2.0.md`](v2.2.0.md) (refreshed by #190) — release body is this file verbatim |
| **Acceptance** | Notes match tip; Draft release held the six images + `SHA256SUMS.txt`; owner Published; never publish with an empty asset list |
| **Agents** | No unilateral tag/Publish; next coding is **M2** (M1 landed) |
| **Published** | https://github.com/squirelworks/squirrelbios/releases/tag/v2.2.0 (`0051c40`, 2026-11-15; six images + `SHA256SUMS.txt` + full notes). An earlier stub without assets was reverted, refilled via the Draft-release asset set, then Published. |
| **Compatibility matrix** | Row: BIOS **v2.2.0** ↔ OS/2 **v2.1.x / tip** (BIOS/CSM/UEFI paths as already documented); cite `uefi-squirrelbios` / `os-boot.sh` when reliable |

**Handshake after M0:**

1. **Owner:** **Published** `v2.2.0` —
   https://github.com/squirelworks/squirrelbios/releases/tag/v2.2.0
2. **`-2eaf`:** **M1** signed capsule landed as #193 on `main`; **M2**
   flash rewrite, **M4** project-key / ESRT, **M5** measured hand-off,
   **M6** os-boot sync, **M7** docs_lint harden, **M3** Setup UI
   polish, **M9** VisionFive2 / HiFive desk notes, **M15** Secure
   Boot everyday path, and **M16** measured-boot polish landed. Optional
   **M8** CI shard is **skipped** while `make test` wall-clock stays
   ~4–5 minutes. **M10** AIA MSI stays blocked on CI QEMU 8.2. Next
   coding is **M14** (banner prep) / **M17+** (not desk M11–M12; those
   are `needs: squirrel`).
3. Soft PATH stays index-only; do not move this ladder into Soft.

---

## Phase 1 — Phase G product remainder (capsule → measured hand-off)

Lanes: **only `-2eaf`** for code. Owner marks wait on `needs: squirrel`.

### M1 — `2.2.1` signed capsule verify + apply  ·  **done (#193)**  ·  depends: M0

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.1` (landed on `main` via #193; patch tag optional) |
| **Box** | `G-capsule-signed` / TODO Phase G follow-up after #189 |
| **PR** | [#193](https://github.com/squirelworks/squirrelbios/pull/193) merged |
| **Suggested check** | `x86_64-capsule-signed`, `x86_64-capsule-fwcfg` |
| **Design note** | `release-notes/capsule-signed-apply.md` |
| **Acceptance** | Detached PKCS#7 / `WIN_CERTIFICATE_UEFI_GUID` verified against a project capsule-db (fw_cfg stand-in OK); apply updates ESRT `LastAttempt*`; bad signature refused with a clear serial line and halt/return style matching existing capsule failures; `make test` + `make docs-lint` green; epic #82 comment |
| **Release note** | Short `v2.2.1.md` when owner wants a patch tag; else fold into next mark |
| **Depends** | #189 on `main`; prefer after M0 so the published 2.2.0 story stays stable |
| **Lane** | `-2eaf` |

### M2 — `2.2.25` capsule flash rewrite (QEMU/pflash)  ·  **done**  ·  depends: M1

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.25` (landed; patch tag optional) |
| **Box** | `G-capsule-flash` |
| **PR** | *(this PR)* `cursor/capsule-flash-rewrite-ebbc` |
| **Suggested check** | `x86_64-capsule-flash-1`, `x86_64-capsule-flash-2` |
| **Design note** | `release-notes/capsule-flash-rewrite.md` |
| **Acceptance** | Verified apply updates ESRT `FwVersion` and persists `SqFwFlashId` in the variable store (pflash unit 1); unit 0 stays read-only with an explicit “NV stand-in, not real SPI” line on the next boot; never commit firmware images; epic #82 |
| **Lane** | `-2eaf` |

### M3 — `2.2.2` Setup UI polish slice (optional)  ·  **done**  ·  `-2eaf`  ·  depends: M0

| Field | Value |
|-------|-------|
| **Bump** | Historical ladder `2.2.2`; landed after M5/M6 — `SQ_VERSION` stays **2.3.0** (do not renumber history) |
| **Box** | `G-setup-ui-polish` |
| **Issue to file** | *(draft)* `Setup UI polish: remaining notebook / string / ConIn edge cases` |
| **Suggested check** | extend `x86_64-setupui*` or add `x86_64-setupui-polish` |
| **Design note** | `release-notes/setup-ui-polish.md` |
| **Acceptance** | Only if a concrete gap remains after `setup-ui-followups.md`; **skip** if nothing actionable — do not invent chrome |
| **Lane** | `-2eaf` |
| **Status** | **Done** — Main Menu root label; F2 empty BootOrder honesty; smoke `x86_64-setupui-polish` |

### M4 — `2.2.50` project-key capsule signing + ESRT fwupd-shaped path  ·  **done**  ·  depends: M1 (M2 preferred)

| Field | Value |
|-------|-------|
| **Bump** | Mid-minor mark **`2.2.50`** (intentional; landed without waiting on M2 flash rewrite) |
| **Box** | `G-capsule-project-key` |
| **PR** | *(this PR)* `cursor/capsule-project-key-ebbc` |
| **Suggested check** | `x86_64-capsule-key`, `x86_64-capsule-key-esrt`, `capsule-project-key-host` |
| **Design note** | `release-notes/capsule-project-key.md` |
| **Acceptance** | Public test key + signing recipe in-tree; ESRT fields populated so a host-side stand-in can list/update; QEMU-only; epic #82 |
| **Lane** | `-2eaf` |

### M5 — `2.3.0` measured hand-off spine (TPM event log → OS)  ·  `-2eaf`  ·  depends: M0; coordinates with OS/2 later  ·  **done**

| Field | Value |
|-------|-------|
| **Bump** | `+0.25.0`-class jump to **`2.3.0`** — “measured boot is a product surface” |
| **Box** | `G-measured-handoff` |
| **Issue to file** | *(draft)* `Measured hand-off: pass TPM event log to Squirrel-OS/2` |
| **Suggested check** | BIOS: `x86_64-tpm-log-handoff` (serial / configuration table / hand-off blob per `common/handoff.md`); OS side later: coordinate name with OS/2 `regress.sh` (e.g. `tpm-eventlog` — **do not invent the OS check alone**) |
| **Design note** | `release-notes/measured-handoff.md` + `common/handoff.md` contract update |
| **Acceptance** | With `swtpm`, firmware exposes the TCG2 event log to the OS stand-in; OS/2 consumption may lag — BIOS mark closes when the **firmware contract** is tested; note the OS follow-up in HANDOFF |
| **Release note** | Full `v2.3.0.md` + compatibility matrix row vs OS/2 tags |
| **Lane** | `-2eaf` (BIOS); OS page is OS/2 lane when ready |
| **Status** | **Done** — SQTL config table + `x86_64-tpm-log-handoff`; owner publishes `v2.3.0` when ready |

### M6 — `2.3.1` os-boot.sh keep-in-step  ·  `-2eaf`  ·  depends: M0; track OS/2 releases  ·  **done**

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.1` |
| **Box** | `os-boot-sync` |
| **Issue to file** | *(draft)* `tests/os-boot.sh: track squirrel-os2 release tags / tip` |
| **Suggested check** | existing `os-boot` / `uefi-squirrelbios` paths stay green against a pinned OS/2 artifact or documented tip |
| **Design note** | `release-notes/os-boot-sync.md` (short) |
| **Acceptance** | Script and HANDOFF cite the OS/2 tag used; failure message names the mismatch |
| **Lane** | `-2eaf` |
| **Status** | **Done** — pin `tests/os-boot.pin` → OS/2 **`v2.3.0`**; host smoke `os-boot-sync`; `SQ_VERSION` stays 2.3.0 |

---

## Phase 2 — Infrastructure (CONTINUANCE 3c/3d style) + secondary boards

### M7 — `2.3.25` docs_lint allowlist / honesty strengthen  ·  `-2eaf`  ·  depends: M0

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.25` (ladder mark; `SQ_VERSION` stays **2.3.0**) |
| **Box** | `docs-lint-harden` |
| **Issue to file** | *(draft)* `docs_lint: tighten allowlist; refuse stale Phase G / version claims` |
| **Suggested check** | `make docs-lint`; host smoke `docs-lint-harden` (`--self-test` negative fixture) |
| **Design note** | `release-notes/docs-lint-harden.md` |
| **Acceptance** | Allowlist shrinks or gains audited entries; ROADMAP Phase E banner note (historical vs product 3.0.0) stays consistent with this file |
| **Lane** | `-2eaf` |
| **Status** | **Done** — unused allowlist fails; live `SQ_VERSION` + Phase E banner checks; `docs-lint-harden` host smoke |

### M8 — `2.3.50` shard `make test` CI (optional)  ·  **skipped (wall-clock)**  ·  `-2eaf`  ·  depends: M7 recommended

| Field | Value |
|-------|-------|
| **Bump** | Mid-minor **`2.3.50`** |
| **Box** | `ci-shard-smoke` |
| **Issue to file** | *(draft)* `CI: shard tests/smoke.sh without weakening the required gate` |
| **Suggested check** | workflow still requires the full logical gate; shards only for wall-clock |
| **Design note** | `release-notes/ci-shard-smoke.md` |
| **Acceptance** | `.github/workflows/make-test.yml` green; no case silently dropped; document how to run one shard locally |
| **Lane** | `-2eaf` |
| **Skip if** | wall-clock is already acceptable — do not shard for fashion |
| **Status** | **Skipped** after M3/M9 — `make-test` on `main` completes in ~4–5 minutes; revisit only if the suite grows enough that the gate hurts |

### M9 — `2.3.2` VisionFive2 / HiFive desk notes (optional)  ·  **done**  ·  `-2eaf`  ·  depends: M0

| Field | Value |
|-------|-------|
| **Bump** | Ladder **`2.3.2`** (docs completeness; not a jump to `2.4.0`); `SQ_VERSION` stays **2.3.0** |
| **Box** | `board-vf2-hifive-notes` |
| **Issue to file** | *(draft)* `VisionFive2 / HiFive: desk boot notes beside existing CONFIG=` |
| **Suggested check** | keep existing build-check / `sifive_u` smoke; desk results are post-merge notes |
| **Design note** | `boards/visionfive2/DESK.md`, `boards/hifive-unmatched/DESK.md`, `release-notes/board-vf2-hifive-notes.md` |
| **Acceptance** | CONFIG already on `main`; this mark is **docs + optional desk**; no QEMU JH7110 machine required |
| **Lane** | `-2eaf` for docs; desk flash observations may be `needs: squirrel` |
| **Status** | **Done** — desk procedures landed; CI keeps `riscv64-visionfive2-build` + HiFive build/`sifive_u`; desk flash still `needs: squirrel` |

### M10 — `2.4.1` RISCV-AIA-MSI  ·  `-2eaf`  ·  **blocked today** on QEMU

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.1` |
| **Box** | `riscv-aia` / `RISCV-AIA-MSI` |
| **Issue to file** | *(draft)* `RISC-V AIA MSI: smsiaddrcfg on QEMU ≥ 9.1 → linux-big shell` |
| **Suggested check** | `LINUX_AIA=imsic make linux-big` (optional suite) + smoke note in HANDOFF |
| **Design note** | `release-notes/riscv-aia-msi.md` |
| **Acceptance** | Program `smsiaddrcfg`/`smsiaddrcfgh`; APLIC-direct remains green; MSI mode reaches a shell on CI’s QEMU when unblocked |
| **Blocked** | QEMU 8.2 MSI storms — **do not force** on old CI images; bump the workflow package when ready |
| **Lane** | `-2eaf` |
| **Status** | **Blocked** on CI QEMU 8.2 (this environment reports 8.2.2); skip until ≥ 9.1 is the CI package |

### M11 — `2.4.2` Pi 4 desk flash  ·  **needs: squirrel**  ·  depends: QEMU `rpi4-uefi-sd` (done)

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.1` |
| **Box** | `D2-rpi4` desk |
| **Issue to file** | *(draft)* `D2: desk Raspberry Pi 4 SD boot of aarch64-uefi` |
| **Acceptance** | Serial reaches `disk: SD card` and `efi: ExitBootServices` per `boards/rpi4/README.md`; post-merge note only |
| **Lane** | `needs: squirrel` |

### M12 — `2.4.3` GA-G41M desk flash  ·  **needs: squirrel**  ·  depends: `CONFIG=coreboot` (done)

| Field | Value |
|-------|-------|
| **Bump** | `+0.0.1` |
| **Box** | `C5-board` desk |
| **Issue to file** | *(draft)* `C5: desk GA-G41M-ES2L flash with coreboot + SquirrelBIOS payload` |
| **Acceptance** | Serial shows `mem: … (from coreboot tables)` per `boards/ga-g41m-es2l/FLASHING.md` |
| **Lane** | `needs: squirrel` |

### M13 — `2.5.0` Phase D “full firmware” banner (optional product mark)  ·  **shared / owner**  ·  depends: M9–M12 as desired

| Field | Value |
|-------|-------|
| **Bump** | `+0.25.0`-class → **`2.5.0`** matching ROADMAP Phase D banner |
| **Why** | ROADMAP historically labeled Phase D as “v2.5: ARM and RISC-V as full firmware”. QEMU work is largely done; this tag **declares** product completeness under QEMU + recorded desk notes — **not** a redo of D1–D4 |
| **Acceptance** | Notes `v2.5.0.md`; matrix rows for aarch64/riscv64 UEFI; open desk items listed honestly if still pending |
| **Lane** | agents prep notes (`-2eaf`); owner tags |

---

## Phase 3 — Product polish → verified chain → `3.0.0`

### M14 — `2.6.0` Phase G product “capsule + setup done” banner  ·  `-2eaf` / owner  ·  depends: M2 + M4 (+ M3 if taken)

| Field | Value |
|-------|-------|
| **Bump** | Jump to **`2.6.0`** |
| **Box** | Tick remaining Phase G capsule/setup boxes on #82 |
| **Acceptance** | Signed apply + flash path + project-key story documented; Setup polish either done or explicitly declined in HANDOFF |
| **Release note** | `v2.6.0.md` + compatibility matrix |
| **Lane** | `-2eaf` prep; owner tag |

### M15 — `2.6.1` Secure Boot everyday path polish  ·  **done**  ·  `-2eaf`  ·  depends: M14 recommended

| Field | Value |
|-------|-------|
| **Bump** | Ladder **`2.6.1`**; `SQ_VERSION` stays **2.3.0** until M14 cuts `2.6.0` |
| **Box** | `G-sb-everyday` |
| **Issue to file** | *(draft)* `Secure Boot: everyday enroll/boot path for Squirrel-OS/2 loader under QEMU` |
| **Suggested check** | `x86_64-sb-os2`, `x86_64-sb-os2-esp`, `x86_64-sb-os2-unsigned` |
| **Design note** | `release-notes/sb-everyday.md` |
| **Acceptance** | With SB on, only project-signed loader starts; unsigned refused; TPM extends still green |
| **Lane** | `-2eaf` |
| **Status** | **Done** — signed OS/2 loader stand-in + ESP path + unsigned refuse; tcglog replay green |

### M16 — `2.7.0` measured boot polish (PCR policy / log format)  ·  **done**  ·  `-2eaf`  ·  depends: M5

| Field | Value |
|-------|-------|
| **Bump** | Ladder **`2.7.0`**; `SQ_VERSION` stays **2.3.0** until M14 cuts `2.6.0` |
| **Box** | `G-tpm-polish` |
| **Issue to file** | *(draft)* `TPM measured boot polish: stable event log fields for OS Control Panel` |
| **Suggested check** | `x86_64-tpm-polish`, extended `x86_64-tpm-log-handoff` |
| **Design note** | `release-notes/tpm-polish.md` |
| **Acceptance** | Event log fields documented in `common/handoff.md`; OS can parse without firmware private headers |
| **Lane** | `-2eaf` |
| **Status** | **Done** — SQTL `ActiveBanks` + PCR policy v1 / TCG2 field layout; stand-in `banks=ok parse=ok policy=ok` |

### M17 — `2.8.0` suite matrix hardening (BIOS ↔ OS/2 tags)  ·  `-2eaf`  ·  depends: M6 + recent OS/2 tags

| Field | Value |
|-------|-------|
| **Bump** | Jump to **`2.8.0`** |
| **Box** | `compat-matrix` |
| **Issue to file** | *(draft)* `Release notes: compatibility matrix vs OS/2 tags (BIOS/CSM/UEFI)` |
| **Suggested check** | docs_lint or a small table test that cited tags exist |
| **Design note** | every `release-notes/v*.md` from here on includes the matrix section |
| **Acceptance** | Matrix lists BIOS tag, OS/2 tag, path (legacy BIOS / CSM / UEFI), QEMU machine |
| **Lane** | `-2eaf` |

### M18 — `2.9.0` pre-3.0 verified-chain rehearsal  ·  `-2eaf`  ·  depends: M15 + M16; OS/2 2.9+ when available

| Field | Value |
|-------|-------|
| **Bump** | Jump to **`2.9.0`** (deliberate parallel numbering with OS/2 2.9 bridge — **companion rehearsal**, not Phase E) |
| **Box** | `G-m3-rehearsal` |
| **Issue to file** | *(draft)* `M3 rehearsal: SB on + TPM log + signed OS loader under QEMU` |
| **Suggested check** | `x86_64-m3-rehearsal` (aggregate smoke) |
| **Design note** | `release-notes/m3-rehearsal.md` |
| **Acceptance** | One QEMU recipe boots a signed OS/2 (or stand-in) with SB+TPM; serial shows measure + hand-off lines; gaps listed honestly |
| **Lane** | `-2eaf` |

### M19 — `2.9.1` … `2.9.25` buffer marks  ·  `-2eaf`  ·  as needed

Use **`+0.0.1` / `+0.0.25`** freely for:

- Sarah follow-ups on Setup / capsule / TPM wording
- Extra fuzz corpora that unblock a product claim
- `os-boot.sh` bumps when OS/2 cuts 2.9.x / 3.0.0-rc tags
- Docs-only honesty PRs

Do not burn a minor bump for typos.

### M20 — `3.0.0` ship  ·  **needs: squirrel**  ·  depends: M14–M18 (M11–M13 desk optional, not ship-blocking)

| Field | Value |
|-------|-------|
| **Bump** | Major **`3.0.0`** — CONTINUANCE **M3** companion to OS/2 **3.0.0** |
| **Banner / `SQ_VERSION`** | `3.0.0` on UEFI (`common/efi/include/sq.h`); align platform banners that still say `3.0.0-alpha` only when those platforms are intentionally in the ship set |
| **Notes** | Full `v3.0.0.md`; suite matrix with OS/2 **3.0.0**; Soft PATH index may link here |
| **Acceptance** | Verified chain under QEMU: Secure Boot + TPM measure + event-log hand-off + signed capsule path; Phase G product boxes on #82 ticked or explicitly deferred with owner OK; `make test` green on the tagged commit; owner Publishes |
| **Non-goals** | Redoing Phase E; requiring Pi 4 / GA-G41M desk success; requiring AIA MSI if CI QEMU still blocked |
| **Agents** | Prepare notes + prefilled link; **never tag** |

---

## Phase 4 — Parallel / later (do not block 3.0.0 unless owner expands the gate)

| Mark | Title | Lane | Depends | Acceptance |
|------|-------|------|---------|------------|
| later | Pi 5 research → CONFIG | `-2eaf` | M11 optional | design note under `boards/` |
| later | F4-scale in optional nightly | `-2eaf` | — | `make f4-scale` documented |
| later | More host fuzz targets | `-2eaf` | — | `make fuzz-*` + optional workflow |
| later | Real SPI capsule on desk hardware | `needs: squirrel` | M2 | post-merge note |
| later | OS/2 Control Panel “boot security” page | OS/2 lanes | M5 + OS/2 3.0 work | OS regress check |
| 3.x | Additional fwupd integration beyond QEMU | shared | M20 | owner scope |

---

## Compatibility matrix (paste into release notes)

Every BIOS release note from **v2.2.0** onward should carry a short table.
Fill real SHAs/tags at publish time:

| BIOS tag | OS/2 tag (min) | Path | QEMU machine | Check / script |
|----------|----------------|------|--------------|----------------|
| v2.1.1 | v2.1.x / tip | legacy BIOS, CSM, UEFI | `pc` / `q35` | `tests/os-boot.sh`, OS `uefi-squirrelbios` |
| v2.2.0 | OS/2 tip at cut | same + capsule/ESRT present | `q35` + swtpm for TPM cases | `x86_64-capsule`, Setup UI cases |
| v2.3.0 | OS/2 with event-log consumer *(when ready)* | UEFI + TPM | `q35` + swtpm | `x86_64-tpm-log-handoff` + OS name TBD |
| v2.3.1 *(mark; SQ_VERSION still 2.3.0)* | **OS/2 `v2.3.0`** (pinned) | legacy BIOS / CSM `os-boot` | `pc` / `q35` | `os-boot-sync`, `tests/os-boot.sh` |
| v2.9.0 | OS/2 2.9.x | UEFI SB + TPM rehearsal | `q35` + swtpm | `x86_64-m3-rehearsal` |
| **v3.0.0** | **OS/2 3.0.0** | verified chain | `q35` + swtpm | aggregate M20 checks |

Soft does not own rows; it may **index** them.

---

## Resume after credits reset

### In flight (verify with `gh pr list` / HANDOFF before coding)

| Actor | Now | Next |
|-------|-----|------|
| **`-2eaf` (BIOS)** | **M1–M7**, **M3**, **M9**, **M15**, **M16** on `main` — **do not redo**; **M8** skipped; **M10** blocked | **M14** banner prep / **M17+** |
| **Owner** | **M0** `v2.2.0` published | Later M13/M14/M20; desk M11/M12 when hardware is free |
| **`-b55f`** | **Not on BIOS** — OS/2 2.9 only | Ignore this file for OS claims; see OS/2 `TO_3.0.0.md` |
| **Soft** | PATH index only | Do not move BIOS marks into Soft |

### Cold-start checklist (BIOS agent)

1. `git fetch origin main && git checkout main && git pull`
2. Read **HANDOFF.md** (Suggested order + Path to 3.0.0 pointer) and **this file’s Resume table**
3. `gh pr list --repo squirelworks/squirrelbios` — confirm #190 / capsule PRs
4. Comment on epic [#82](https://github.com/squirelworks/squirrelbios/issues/82) with the mark you take (M1, M2, …)
5. Branch `cursor/<box>-2eaf` from `main` only (suffix `-2eaf`; never `-b55f` on BIOS)
6. Land code + `make test` case + design note + TODO/ROADMAP/#82 ticks in the **same** PR
7. Prepare `release-notes/vX.Y.Z.md` when the mark is a cut; owner tags

### Do not

- Steal OS/2 `-b55f` boxes or open BIOS PRs already in flight
- Tag or Publish releases
- Stack PRs on feature branches
- Redefine **v3.0.0** as “finish Phase E” (Phase E is done)
- Hold **M0** forever for signed-capsule if #190 is ready — use **M1** instead
- Commit firmware images, private keys, or real-board dumps

---

## Issue filing list (for Sarah / parent if agents get 403)

Titles only; file under `squirelworks/squirrelbios` and link epic #82:

1. `G-capsule-signed: PKCS#7 verify + UpdateCapsule apply / PersistAcrossReset staging` (`-2eaf`)
2. `G-capsule-flash: rewrite firmware image under QEMU pflash after signed apply` (`-2eaf`)
3. `Setup UI polish: remaining notebook / string / ConIn edge cases` (`-2eaf`, optional)
4. `Capsule signing with project key; ESRT fwupd-shaped path under QEMU` (`-2eaf`)
5. `Measured hand-off: pass TPM event log to Squirrel-OS/2` (`-2eaf`)
6. ~~`tests/os-boot.sh: track squirrel-os2 release tags`~~ (`-2eaf`, **done** — M6 / `os-boot-sync`)
7. ~~`docs_lint: tighten allowlist; refuse stale version claims`~~ (`-2eaf`, **done** — M7 / `docs-lint-harden`)
8. `CI: shard tests/smoke.sh without weakening the required gate` (`-2eaf`, optional)
9. `RISC-V AIA MSI: smsiaddrcfg on QEMU ≥ 9.1` (`-2eaf`, blocked)
10. `D2: desk Raspberry Pi 4 SD boot` (`needs: squirrel`)
11. `C5: desk GA-G41M-ES2L flash` (`needs: squirrel`)
12. `Secure Boot everyday path for Squirrel-OS/2 loader` (`-2eaf`)
13. `TPM measured boot polish for OS Control Panel` (`-2eaf`)
14. `M3 rehearsal: SB + TPM + signed loader under QEMU` (`-2eaf`)
15. `Path to 3.0.0 walkthrough` (this doc — tracker / epic link)

Keep [#82](https://github.com/squirelworks/squirrelbios/issues/82) as the **Phase G / product epic**; do not dump the full ladder into the epic body — point here.

---

## Ladder at a glance

```
[#190 docs refresh]  (+ signed-capsule WIP? → prefer M1)
        │
        ▼
   v2.2.0 tip cut (owner)                         ← M0
        │
        ├─ 2.2.1 signed capsule verify/apply      ← M1  (-2eaf)
        │      │
        │   2.2.25 flash rewrite                  ← M2  (-2eaf)
        │      │
        │   2.2.50 project key / fwupd-shaped     ← M4  (-2eaf)
        │
        ├─ 2.2.2 Setup polish (optional)          ← M3  (-2eaf)
        │
        ├─ 2.3.0 measured hand-off                ← M5  (-2eaf)
        │      └─ 2.3.1 os-boot.sh sync            ← M6
        │
        ├─ 2.3.25 docs_lint / 2.3.50 CI shard     ← M7 / M8
        │
        ├─ 2.4.x boards + AIA (AIA blocked)       ← M9–M12
        │      └─ 2.5.0 Phase D banner (optional) ← M13 (owner)
        │
        ├─ 2.6.0 Phase G product banner           ← M14
        │      └─ 2.6.1 SB everyday               ← M15
        │
        ├─ 2.7.0 TPM polish                       ← M16
        ├─ 2.8.0 compat matrix harden             ← M17
        ├─ 2.9.0 M3 rehearsal (+ 2.9.x buffer)    ← M18 / M19
        │
        ▼
   v3.0.0 CONTINUANCE M3 companion (owner)        ← M20
```

**Semantics reminder:** ROADMAP Phase E’s historical “v3.0” label ≠ product
**v3.0.0**. Product **v3.0.0** is the verified-chain companion to OS/2 3.0.0.
