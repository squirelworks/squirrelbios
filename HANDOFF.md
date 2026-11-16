# Hand-off to Cursor / Codex (2026-11-14)

From 2026-11-14 the remaining SquirrelBIOS work is left for Cursor or Codex to
pick up. This file says what is done, what is in flight, and what is open,
with enough detail to start any open item cold. The suite-wide plan for what
comes after this list is
[squirrelsoft `docs/CONTINUANCE.md`](https://github.com/squirelworks/squirrelsoft/blob/main/docs/CONTINUANCE.md).

**Path to 3.0.0:** the fine-grained BIOS ladder (M0…M20, version bumps,
resume-after-credit-reset, Phase E vs product v3.0.0 semantics) lives in
[`release-notes/TO_3.0.0.md`](release-notes/TO_3.0.0.md). Soft PATH is suite
index only; lane **`-2eaf`** owns BIOS marks (no `-b55f` on this repo).

Source of truth, in order: `ROADMAP.md` and `TODO.md` on `main`, then the
GitHub issues (epic #82, hand-off tracker), then this file. If they
disagree, `main`'s code and `tests/smoke.sh` win; fix the docs in your PR.

## Status legend

Used here, in `ROADMAP.md` and on the issues (as labels).

| Mark | Label | Meaning |
|------|-------|---------|
| `- [x]` | `status: done` | On `main` with a `make test` case. |
| *(in progress)* | `status: in progress` | A branch or PR exists. Finish it; do not redo it. |
| `- [ ]` | `cursor-codex` | Open. Nobody is on it. Take it. |
| *(Squirrel)* | `needs: squirrel` | Only the owner can do it (publish a release, flash a real board). |

## Where things are

- **Released:** **v2.2.0** (2026-11-15,
  https://github.com/squirelworks/squirrelbios/releases/tag/v2.2.0 ,
  tip `0051c40`). Prior: v2.1.1 (2026-11-13).
- **`main` after v2.2.0:** D4 shared drivers, network/HTTP boot, NVRAM +
  authenticated variables, TPM, SMM/EINJ, IPMI, all of Phase E, Raspberry Pi
  on QEMU, F1–F4 as tested, Phase G Setup UI + host fuzz + UpdateCapsule/ESRT
  (#189), signed capsule apply (#193) + Sarah follow-ups (#194),
  capsule flash rewrite via NV identity (M2), project-key / ESRT
  fwupd-shaped path (M4), measured TPM event-log hand-off
  (M5 / SQTL), os-boot sync (M6 — pinned OS/2 **`v2.3.0`**),
docs_lint harden (M7), Setup UI polish (M3 — Main Menu label +
  empty BootOrder honesty), and VisionFive 2 / HiFive desk notes (M9).
  Licence GPL-3.0. Notes: `release-notes/v2.2.0.md`,
  `release-notes/v2.3.0.md`, `release-notes/os-boot-sync.md`,
  `release-notes/docs-lint-harden.md`,
  `release-notes/setup-ui-polish.md`,
  `release-notes/board-vf2-hifive-notes.md`.
  Optional **M8** CI shard is **skipped** while `make test` stays
  ~4–5 min wall-clock. **M10** AIA MSI stays **blocked** on CI QEMU
  8.2. Next coding: **M14** / **M15** (desk M11–M12 remain
  `needs: squirrel`).
- **Test gate:** `make test` (docs honesty via `make docs-lint` /
  `tools/docs_lint.py` + host `docs-lint-harden`, then ~350 cases in
  `tests/smoke.sh`, plus
  `rpi4-uefi-sd` when QEMU has `raspi4b`), run by
  `.github/workflows/make-test.yml` on every PR. Optional, not in the
  required gate: `make linux-x86`, `make linux-uefi`, `make linux-shell`,
  `make linux-big`, `make f4-scale`, `make fuzz-fat` / `make fuzz-pe` /
  `make fuzz-authenticode` / `make fuzz-net` / `make fuzz-usb` /
  `make fuzz-fdt` (Phase G host FAT, PE/COFF, Authenticode/PKCS#7,
  DHCP/TFTP/HTTP, USB descriptor and FDT corpus smokes; optional
  path-filtered workflows `.github/workflows/fuzz-*.yml` and scheduled
  `fuzz-nightly.yml`).
- **Open PRs at hand-off:** path-to-3.0.0 docs (#192) if still open; board
  flashes stay `needs: squirrel`.
- **Agent lanes:** suite split in squirrelsoft
  [`docs/AGENT_LANES.md`](https://github.com/squirelworks/squirrelsoft/blob/main/docs/AGENT_LANES.md).
  Lane `-2eaf` may take free BIOS `cursor-codex` follow-ups; lane `-b55f` is OS/2 2.9.

## Rules (Squirrel's; do not break)

1. Base every PR on `main`. Never stack a branch on another feature branch.
   Group related steps into one sizeable PR. Before finishing, confirm the PR
   targets `main` and GitHub shows it mergeable with `make-test` green.
2. **Original code only.** Do not vendor or copy SeaBIOS, EDK2/OVMF, OpenSBI,
   U-Boot, coreboot, OpenBIOS, SLOF, Linux or any other firmware. Public
   specifications (UEFI, ACPI, PI, PCI, SBI, IEEE 1275, TCG) are fine; cite them.
3. Every new behaviour lands with a `make test` case (stand-in image under
   `tests/`, a rule in the Makefile, a `run` line in `tests/smoke.sh`) and an
   update to `common/handoff.md` / the platform README when visible behaviour changes.
4. Failures are a message and a halt, in the existing style
   (`disk: ...`, `boot: ...`, then `boot halted`). Never jump to a partial image.
5. Images never go in the tree. Only Squirrel tags and publishes: hand over
   finished release notes in `release-notes/v<version>.md`, the draft made by
   `Actions → Draft release`, and one prefilled release link.
6. Real hardware checks are post-merge notes, never a merge blocker.
7. Never merge `klingon-font*` branches. Never delete branches.

## Build and test

```sh
sudo apt-get install make nasm gcc gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu \
  qemu-system-x86 qemu-system-arm qemu-system-misc swtpm swtpm-tools \
  gcc-powerpc64le-linux-gnu qemu-system-ppc gcc-sparc64-linux-gnu qemu-system-sparc \
  binutils-loongarch64-linux-gnu binutils-mips-linux-gnu qemu-system-mips \
  gcc-m68k-linux-gnu gcc-s390x-linux-gnu qemu-system-s390x python3
                                # the same list as .github/workflows/make-test.yml
make                            # every platform's default config
make PLATFORM=x86_64-uefi CONFIG=default
make test                       # all cases; prints PASS/FAIL per case
```

To add a case, copy the nearest `run` line in `tests/smoke.sh`: it takes the
expected serial lines (`|`-separated, `!` = must not appear, `~` = regex).

## Suggested order

Full mark list (versions, acceptance, issue titles):
[`release-notes/TO_3.0.0.md`](release-notes/TO_3.0.0.md). Short form:

1. ~~**REL-2.2.0 / M0**~~ *(published **v2.2.0**, 2026-11-15)* —
   https://github.com/squirelworks/squirrelbios/releases/tag/v2.2.0
   (`0051c40`; six images + `SHA256SUMS.txt` + full notes). Always fill
   via `Actions → Draft release` before Publish — never stub-publish.
2. **Phase G product remainder** (`-2eaf`, **M1–M7** + **M3** + **M9**):
   ~~**M1** signed capsule~~ (#193) → ~~**M2** flash rewrite / NV
   identity~~ → ~~**M4** project-key / ESRT~~ → ~~**M5** measured TPM
   event-log hand-off~~ → ~~**M6** os-boot sync~~ (OS/2 **`v2.3.0`**) →
   ~~**M7** docs_lint harden~~ → ~~**M3** Setup UI polish~~ →
   ~~**M9** VF2 / HiFive desk notes~~; **M8** CI shard **skipped**
   while wall-clock is fine.
3. **D2-rpi4** board flash *(Squirrel, **M11**)*: the QEMU `raspi4b` case
   is done; try the SD layout in `boards/rpi4/README.md` on a desk Pi 4.
4. **C5-board** desk flash *(Squirrel, **M12**)*: docs and
   `CONFIG=coreboot` are done; flash a real GA-G41M-ES2L per
   `boards/ga-g41m-es2l/FLASHING.md`.
5. **RISC-V AIA MSI** (**M10**) when a newer QEMU (≥ 9.1) is available in
   CI — **blocked** on 8.2 today (do not force).
6. Already on `main` (do not re-cut for these alone): G1–G3 host fuzz,
   Setup UI, UpdateCapsule + ESRT, signed apply / PersistAcrossReset,
   measured hand-off, board desk notes
   (`make fuzz-fat`, `make fuzz-pe`, `make fuzz-authenticode`,
   `make fuzz-net`, `make fuzz-usb`, `make fuzz-fdt`,
`x86_64-setupui`, `x86_64-setupui-f2`, `x86_64-setupui-polish`,
   `x86_64-setupui-bootorder`, `x86_64-setupui-secboot`,
   `x86_64-setupui-datetime`, `x86_64-capsule`, `x86_64-capsule-signed`,
   `x86_64-capsule-fwcfg`, `x86_64-capsule-flash-1`,
   `x86_64-capsule-flash-2`, `x86_64-capsule-key`,
   `x86_64-tpm-log-handoff`, `os-boot-sync`, `docs-lint-harden`,
   `riscv64-visionfive2-build`, `riscv64-hifive-unmatched-build`,
   `riscv64-hifive-unmatched-sifive-u`).
7. Climb **M14–M20** in `TO_3.0.0.md` toward product **v3.0.0**
   (CONTINUANCE M3 companion — **not** a Phase E redo). **M8** only if
   CI wall-clock stops being acceptable. **M10** blocked on QEMU ≥ 9.1.
   Skip #195 unless asked.

## Task cards

### G-board-vf2-hifive-notes — VisionFive 2 / HiFive desk notes (M9)  `status: done`
- **Done (lane `-2eaf`):** desk boot procedures
  `boards/visionfive2/DESK.md` and `boards/hifive-unmatched/DESK.md`
  beside existing `CONFIG=visionfive2` / `CONFIG=hifive-unmatched`.
  Vendor SPL keeps DDR training; our image replaces OpenSBI only.
  Notes: `release-notes/board-vf2-hifive-notes.md`. Epic #82.
  `TO_3.0.0.md` M9 / ladder **2.3.2**. `SQ_VERSION` stays **2.3.0**.
- **Check:** existing `riscv64-visionfive2-build`,
  `riscv64-hifive-unmatched-build`, `riscv64-hifive-unmatched-sifive-u`
  in `make test` (no new QEMU machine).
- **Desk:** real-board flash observations stay `needs: squirrel`
  (append dated notes to each `DESK.md` when run).
- **Next (Phase G):** ~~M3~~ landed; **M8** skipped; **M10** blocked on
  QEMU ≥ 9.1; then **M14** / **M15**. Do not take #195 unless asked.

### G-setup-ui-polish — Setup UI string / notebook polish (M3)  `status: done`
- **Done (lane `-2eaf`):** root Setup panel titled **Main Menu** (serial
  + GOP) so it no longer collides with the Boot Manager notebook item;
  F2 / no-fw_cfg empty BootOrder prints
  `BootOrder empty (demo seed is fw_cfg-only)`. Notes:
  `release-notes/setup-ui-polish.md`. Epic #82. `TO_3.0.0.md` M3.
  `SQ_VERSION` stays **2.3.0**.
- **Check:** `x86_64-setupui`, `x86_64-setupui-f2`,
  `x86_64-setupui-polish` (F2 empty BootOrder), and updated
  `x86_64-setupui-bootorder` in `make test`.
- **Next (Phase G):** ~~M7~~ / ~~M9~~ landed; **M8** CI shard skipped
  for wall-clock (~4–5 min); **M10** blocked; then M14+.

### G-docs-lint-harden — docs_lint allowlist / honesty (M7)  `status: done`
- **Done (lane `-2eaf`):** `tools/docs_lint.py` refuses unused allowlist
  entries, live `SQ_VERSION` claims that disagree with
  `common/efi/include/sq.h`, and a ROADMAP Phase E banner that markets
  historical “v3.0” as product **v3.0.0** without the disclaimer /
  `TO_3.0.0.md` pointer. Negative fixtures via `--self-test`. Notes:
  `release-notes/docs-lint-harden.md`. Epic #82. `TO_3.0.0.md` M7 /
  `2.3.25`. `SQ_VERSION` stays **2.3.0**.
- **Check:** `docs-lint-harden` in `make test` (host; also covered by
  `make docs-lint` on the real tree).
- **Next (Phase G):** ~~M3~~ / ~~M9~~ landed; **M8** skipped; **M10**
  blocked; then M14+.

### G-os-boot-sync — os-boot.sh keep-in-step (M6)  `status: done`
- **Done (lane `-2eaf`):** `tests/os-boot.pin` pins Squirrel-OS/2
  **`v2.3.0`** (`59069e31989828ec5f6973a44f73b31ec04bb035`) and the
  release `os.img` / `uefi.img` SHA-256 digests. `tests/os-boot.sh`
  loads the pin, refuses a mismatched image with a message that names
  the expected tag/tip/SHA vs got, supports `--fetch` and
  `--check-pin`. Notes: `release-notes/os-boot-sync.md`. Epic #82.
  `TO_3.0.0.md` M6 / `2.3.1`. `SQ_VERSION` stays **2.3.0**.
- **Check:** `os-boot-sync` in `make test` (host; no network). Optional
  full boot: `make os-boot` / `tests/os-boot.sh --fetch`.
- **Next (Phase G):** ~~M3~~ / ~~M7~~ / ~~M9~~ landed; **M8** skipped;
  **M10** blocked; then M14+.

### G-measured-handoff — TPM event-log hand-off to OS (M5)  `status: done`
- **Done (lane `-2eaf`):** SQTL configuration table
  (`SQUIRREL_TPM_LOG_HANDOFF_GUID`, magic `'SQTL'`) publishes the TCG2
  event-log physical address / size / last event; agrees with ACPI
  TPM2 and GetEventLog; usable after ExitBootServices. `SQ_VERSION`
  **2.3.0**. Notes: `release-notes/measured-handoff.md`,
  `release-notes/v2.3.0.md`. Epic #82. `TO_3.0.0.md` M5.
- **Check:** `x86_64-tpm-log-handoff` in `make test` (needs swtpm).
- **OS follow-up:** Squirrel-OS/2 boot-security page / regress name —
  coordinate with the OS/2 lane; do not invent alone.
- **Next (Phase G):** ~~M3~~ / ~~M4~~ / ~~M6~~ / ~~M7~~ / ~~M9~~ landed;
  **M8** skipped; **M10** blocked; then M14+.

### G-capsule-flash — flash rewrite via NV identity (M2)  `status: done`
- **Done (lane `-2eaf`):** verified SQCP apply updates ESRT `FwVersion`
  and persists `SqFwFlashId` (`SQFL`) through the variable store.
  QEMU firmware pflash unit 0 stays read-only; unit 1 holds the
  identity (explicit “NV stand-in, not real SPI”). Second boot loads
  the identity before publishing ESRT. Notes:
  `release-notes/capsule-flash-rewrite.md`. Epic #82. `TO_3.0.0.md` M2.
- **Check:** `x86_64-capsule-flash-1`, `x86_64-capsule-flash-2` in
  `make test`.
- **Next (Phase G):** ~~M3~~ / ~~M4~~ / ~~M5~~ / ~~M6~~ / ~~M7~~ / ~~M9~~
  landed; **M8** skipped; **M10** blocked; then M14+.

### G-capsule-project-key — project-key signing + ESRT fwupd-shaped path (M4)  `status: done`
- **Done (lane `-2eaf`):** dedicated capsule project test key
  (seed `0x5153_C001`), `tests/mkcapsule.py --sign-project` /
  `--print-recipe`, trust ESL `capsule-project.esl`, guest ESRT dump
  (`APP esrt-list` / `APP esrt-entry0`) and host
  `tools/esrt_standin.py`. Notes:
  `release-notes/capsule-project-key.md`. Epic #82. `TO_3.0.0.md` M4.
- **Check:** `x86_64-capsule-key`, `x86_64-capsule-key-esrt`,
  `capsule-project-key-host` in `make test`.
- **Next (Phase G):** ~~M2~~ / ~~M3~~ / ~~M5~~ / ~~M6~~ / ~~M7~~ / ~~M9~~
  landed; **M8** skipped; **M10** blocked; then M14+.

### G-capsule-signed — signed capsule apply / PersistAcrossReset staging  `status: done`
- **Done (lane `-2eaf`):** SQCP signed system-firmware capsules
  (detached PKCS#7 / `WIN_CERTIFICATE_UEFI_GUID`) verified against
  fw_cfg `opt/org.squirrelworks/capsule-db` (`x86_64/secure/capsule_auth.c`).
  Immediate apply updates ESRT `LastAttempt*` without rewriting flash;
  `CAPSULE_FLAGS_PERSIST_ACROSS_RESET` stages into NV
  `SqCapsuleStaging` and applies on the next boot (fw_cfg
  `opt/org.squirrelworks/capsule` as a QEMU post-reset stand-in).
  Notes: `release-notes/capsule-signed-apply.md`. Epic #82.
- **Sarah #193 follow-ups (this PR):** multi-PERSIST refused (one NV
  slot); staging capped to `QueryVariableInfo` max (not 1 MB);
  SGL-only UpdateCapsule unsupported (no SUCCESS without verify);
  refused staged apply logs `cleared refused staged capsule`. Notes:
  `release-notes/capsule-signed-followup.md`.
- **Check:** `x86_64-capsule-signed`, `x86_64-capsule-fwcfg` in
  `make test` (plus existing `x86_64-capsule`).
- **Next (Phase G):** ~~flash rewrite~~ / ~~project-key~~ /
~~measured hand-off~~ / ~~os-boot sync~~ / ~~docs_lint~~ /
  ~~Setup polish~~ / ~~M9 desk notes~~ landed; **M8** skipped;
  **M10** blocked; then M14+.

### G-capsule-esrt — UpdateCapsule + ESRT  `status: done`
- **Done (lane `-2eaf`):** UEFI-class capsule plumbing in
  `common/efi/capsule.c` (x86_64 runtime pages + shared DT UEFI
  cores). Installs the EFI System Resource Table (one system-firmware
  resource), implements `UpdateCapsule` /
  `QueryCapsuleCapabilities` (UEFI 2.10 §8.5.3 / §4.6), and publishes
  `OsIndicationsSupported` with the file-capsule bit. QEMU smoke does
  not rewrite flash. Notes: `release-notes/capsule-esrt.md`; also folded
  into `release-notes/v2.2.0.md` for the tag. Epic #82.
- **Check:** `x86_64-capsule` in `make test`.
- **Next:** signed apply in `G-capsule-signed`; flash rewrite in
  `G-capsule-flash`.

### G-setup-ui — Setup UI / boot menu on GOP + F2 + BootOrder + Secure Boot / TPM + date-time / NVRAM  `status: done`
- **Done (lane `-2eaf`):** navigable Warp-look boot menu shell on
  `x86_64-uefi` (`x86_64/core/setupui.c` + `font8x16.h` from the
  original VGA glyphs). Title, Boot Manager list (Continue + Boot
  Manager + Secure Boot keys + TPM clear + Date and time + NVRAM
  reset), Up/Down/Enter/Esc on serial and GOP when Bochs DISPI is
  present. Entered with
  `-fw_cfg name=opt/org.squirrelworks/setupui,string=1` or by pressing
  **F2** during the short post-GOP ConIn window (`setup: press F2 for
  Setup UI`; skipped if ConIn is absent). Boot Manager seeds demo
  `Boot####` / `BootOrder` when empty only under the setupui fw_cfg
  gate, reorders with Left/Right, and `SetVariable`s `BootOrder` only
  when dirty. Secure Boot keys shows PK/KEK/db/dbx status (volatile
  BS|RT demo stubs under fw_cfg; scrubbed on partial seed failure),
  confirms and clears them (clears apply this boot: Setup runs before
  `secure_init`); TPM clear confirms and skips with no TPM (stub when
  a TPM is present - no mid-boot `TPM2_Clear`). Date and time
  views/edits the CMOS RTC (`GetTime` / `SetTime`, including 12h RTC
  Status B); NVRAM reset confirms and clears user NV vars (BootOrder /
  Boot#### / dbt/dbr/MOK-style; fw_cfg-gated demos including a long
  name; leaves Secure Boot PK/KEK/db/dbx; enum grows past 31-char
  names). Root panel is **Main Menu** (M3); empty BootOrder on F2 is
  honest. Menu/printf strings use ASCII `-`. Notes:
  `release-notes/setup-ui-gop.md`, `release-notes/setup-ui-f2.md`,
  `release-notes/setup-ui-bootorder.md`,
  `release-notes/setup-ui-secboot.md`,
  `release-notes/setup-ui-datetime-nvram.md`,
  `release-notes/setup-ui-datetime-followup.md`,
  `release-notes/setup-ui-followups.md`,
  `release-notes/setup-ui-polish.md`, `x86_64/README.md`.
- **Check:** `x86_64-setupui` (fw_cfg), `x86_64-setupui-f2` (F2, no
  fw_cfg), `x86_64-setupui-polish` (F2 empty BootOrder),
  `x86_64-setupui-bootorder` (reorder + save),
  `x86_64-setupui-secboot` (Secure Boot clear + TPM clear, no TPM),
  and `x86_64-setupui-datetime` (RTC edit + NVRAM clear) in
  `make test`.
- **Next (Phase G):** ~~M3~~ / ~~M9~~ landed; fuzz nightly already
  scheduled (`fuzz-nightly.yml`); capsule/ESRT is in `G-capsule-esrt`;
  **M10** blocked; then M14+.

### G1-fuzz-fat — host FAT fuzz / corpus smoke  `status: done`
- **Done:** host-buildable harness for `common/efi/fat.c` under
  `fuzz/` (`LLVMFuzzerTestOneInput`, host EFI stubs, seed corpus).
  `make fuzz-fat` runs a short corpus smoke without QEMU; optional
  `make fuzz-fat-libfuzzer` and `.github/workflows/fuzz-fat.yml`
  (path-filtered / `workflow_dispatch`, not the required gate).
  Notes: `fuzz/README.md`, `release-notes/host-fuzz-fat.md`.
- **Check:** `make fuzz-fat` prints `PASS  fuzz-fat (…)`.

### G2-fuzz-pe — host PE/COFF fuzz / corpus smoke  `status: done`
- **Done:** host-buildable harness for `common/efi/image.c` under
  `fuzz/` (`pe_harness.c`, extended host stubs with pages + protocol
  DB). `make fuzz-pe` runs a short corpus smoke without QEMU; LoadImage
  / UnloadImage only (never StartImage). Optional
  `make fuzz-pe-libfuzzer` and `.github/workflows/fuzz-pe.yml`.
  Notes: `fuzz/README.md`, `release-notes/host-fuzz-pe.md`.
- **Check:** `make fuzz-pe` prints `PASS  fuzz-pe (…)`.

### G3-fuzz-authenticode — host Authenticode/PKCS#7 fuzz / corpus smoke  `status: done`
- **Done (lane `-2eaf`):** host-buildable harness for
  `x86_64/secure/der.c` (PKCS#7 Authenticode + detached, X.509),
  `pe_authenticode_hash` / PE certificate-table walk, and `sb_db_walk`
  under `fuzz/` (`authenticode_harness.c`, `secure_host_stubs.c`,
  synthetic corpus). `make fuzz-authenticode` runs a short corpus
  smoke without QEMU. Optional `make fuzz-authenticode-libfuzzer` and
  `.github/workflows/fuzz-authenticode.yml`. Notes: `fuzz/README.md`,
  `release-notes/host-fuzz-authenticode.md`. Epic #82.
- **Check:** `make fuzz-authenticode` prints
  `PASS  fuzz-authenticode (…)`.

### G3-fuzz-net-usb-fdt — host DHCP/TFTP/HTTP, USB, FDT fuzz  `status: done`
- **Done (lane `-2eaf`):** host-buildable harnesses for
  `common/efi/net_parse.c` (DHCP/TFTP/HTTP), `common/efi/usb_desc.c`
  (device + BBB config descriptors), and `common/fw/fdt.c` /
  `common/efi/fdt.c`. Synthetic corpora under `fuzz/corpus-net/`,
  `corpus-usb/`, `corpus-fdt/`. `make fuzz-net` / `fuzz-usb` /
  `fuzz-fdt` run short corpus smokes without QEMU. Optional
  `make fuzz-*-libfuzzer`, path-filtered workflows, and scheduled
  `.github/workflows/fuzz-nightly.yml` (60 s budget). Notes:
  `fuzz/README.md`, `release-notes/host-fuzz-net-usb-fdt.md`. Epic #82.
- **Check:** `make fuzz-net`, `make fuzz-usb`, `make fuzz-fdt` each
  print `PASS  fuzz-… (…)`.

### REL-2.2.0 — release notes for what `main` has since v2.1.1  `status: done` (published v2.2.0)
- **Done (notes + assets):** `release-notes/v2.2.0.md` (style of
  `v2.1.0.md`; Known gaps refreshed; UEFI banner `SQ_VERSION` `2.2.0`).
  Refreshed after #189 so UpdateCapsule + ESRT / `x86_64-capsule` (and
  Phase G Setup UI + host fuzz) are listed as **in the v2.2.0 tag**, not
  a follow-up cut. `make test` count: **346** on the #189 tip. Release
  holds the six images + `SHA256SUMS.txt` + the full notes body (same
  set `.github/workflows/release-draft.yml` attaches).
- **Done (Squirrel):** published
  https://github.com/squirelworks/squirrelbios/releases/tag/v2.2.0
  (2026-11-15, `0051c40`). Signed apply landed as **M1** / #193 on
  `main` (not a v2.2.1 re-cut of #189 alone). Always run
  `Actions → Draft release` before Publish — never stub-publish empty.
- **Check:** `make test` green on the tagged commit; every image boots its
  `make test` cases; `SHA256SUMS.txt` covers every attached image.

### F2-CXL — CXL memory enumeration  `status: done`
- **Done:** on `x86_64-uefi` with `-M q35,cxl=on` and a `pxb-cxl` host
  bridge + `cxl-rp` + `cxl-type3` device, the firmware finds the CXL host
  bridge (PCI `1b36:000b`), publishes a CEDT (CHBS + CFMWS from QEMU's
  `etc/acpi/tables`) and `ACPI0016`/`ACPI0017` in the DSDT.
  `x86_64-cxl` / `x86_64-cxl-none` in `tests/smoke.sh`.

### F3-DNS — names in HTTP boot  `status: done`
- **Done:** DNS A-record client in the UEFI network stack (`common/efi/net_dns.c`);
  DHCP option 6 supplies the resolver. HTTP boot accepts `http://name/...`
  URLs. `tests/netsim.py` / `tests/httpd.py` answer DNS for the smoke net.
  Cases: `x86_64`/`aarch64`/`riscv64`-uefi-httpboot-dns and
  `x86_64-uefi-httpboot-dns-miss` (`net: cannot resolve …`, fall through).

### D2-rpi4 — Raspberry Pi 4 build on QEMU and on the board  *(QEMU done; board check `needs: squirrel`)*
- **Done:** `configs/aarch64-uefi/rpi3.cfg` and `rpi4.cfg`, SDHCI driver,
  EL3 + PSCI, `rpi3-uefi-sd` on QEMU `raspi3b` (#157); `rpi4-uefi-sd` on
  QEMU `raspi4b` (QEMU ≥ 9.0; skipped cleanly on older QEMU) with BCM2711
  EMMC2 + GIC-400 from `tests/mkrpidtb.py --rpi4`; SD layout / `config.txt`
  / `kernel=` notes in `boards/rpi4/README.md`. Pi 5 stays research.
- **Open (Squirrel):** flash a real Pi 4 with that SD layout and confirm
  the serial log reaches `disk: SD card` and `efi: ExitBootServices`.
- **Acceptance (QEMU):** under `raspi4b` the firmware reaches the EFI
  stand-in with `disk: SD card` (Arasan host at `0xfe300000`; EMMC2 is
  probed and empty under QEMU) and `ExitBootServices`, like
  `rpi3-uefi-sd`. GIC-400 at `0xff841000` is taken from the tree.

### F1-scale — more than 1000 CPUs and several ECAM segments  `status: done`
- **Done:** topology-driven MADT/SRAT/MCFG builders in `x86_64/acpi/topo.c`
  (`topo_impl.inc`); host tool `tests/x86_64-tables` feeds synthetic
  topologies; `tests/acpicheck.py` checks checksums, 1024 distinct x2APIC
  IDs with SRAT affinity per node (`x86_64-tables-1024cpu`), and two MCFG
  allocations (`x86_64-tables-2seg`). Optional KVM run on a big host stays
  a post-merge note.

### C5-board — real x86 hardware  *(docs + CONFIG done; board flash `needs: squirrel`)*
- **Done (docs/CONFIG):** `boards/ga-g41m-es2l/FLASHING.md` (external SPI
  programmer, vendor backup/restore, DualBIOS and clip recovery);
  `CONFIG=coreboot` (`configs/x86_64-uefi/coreboot.cfg`) reads the memory
  map and ACPI RSDP from coreboot LBIO tables (`x86_64/platform/coreboot.c`);
  `x86_64-coreboot` in `tests/smoke.sh` with synthetic tables from
  `tests/mkcbtables.py`. Research note: `boards/ga-g41m-es2l/README.md`.
- **Open (Squirrel):** flash a real GA-G41M-ES2L per the guide with
  coreboot + this payload; confirm serial shows
  `mem: … (from coreboot tables)`. DDR training stays research.

### RISCV-AIA-MSI — Linux on `virt,aia=aplic-imsic`  `cursor-codex` (blocked on QEMU)
- **Background:** TODO.md `riscv-aia`: APLIC direct mode works; MSI mode storms on QEMU 8.2.
- **Open:** program `smsiaddrcfg`/`smsiaddrcfgh` in the M-level APLIC, retest with
  QEMU ≥ 9.1 and a 6.14+ kernel; `LINUX_AIA=imsic make linux-big` reaches the shell.

### F4-scale — more diskless nodes and a large NUMA node  `status: done`
- **Done:** optional `make f4-scale` (`tests/f4-scale.sh`): eight x86_64
  UEFI nodes netboot the same image from one netsim server; node 0 is
  `-smp 256` with four NUMA nodes (falls back to 255 under TCG without
  x2APIC). Not in `make test` / CI. `NETSIM_IDLE` keeps the server up
  while the large node brings up its APs.

### H-avionics — ISM bring-up / BIT / boot integrity (#195)  `cursor-codex`
- **Goal:** Reuse SquirrelBIOS as bring-up / BIT / boot-integrity for
  ISM fighter-jet bay I/O and FCC boards, and Eric’s ISM RC flight
  controller. Laws stay on a separate RTOS. Contract:
  ISM `firmware-bringup.yaml` BIT line.
- **Done:** gap list; BIT; integrity; hand-off; RC first target;
  porting guide `boards/ism-avionics/PORTING.md`. ROADMAP H0–H5 /
  TODO §14 complete for the portable layer.
- **Open:** MCU `PLATFORM=` when RC silicon is pinned; bay/FCC board
  cards as follow-ons.
- **Check:** `avionics-bit`, `avionics-integrity`, `avionics-handoff`,
  `avionics-rc-fc` (host); docs-lint green.
- **Related:** squirrel-os2 #225 (ground/lab); ISM #84 bay firmware.

### Secondary track and small follow-ups  `cursor-codex`
- `riscv-sparse-harts` only if a board with gapped hart IDs is targeted (Speculative in TODO.md).
- VisionFive 2 / HiFive Unmatched (**M9 done**): CONFIG builds and CI
  checks as before; desk procedures in `boards/visionfive2/DESK.md` and
  `boards/hifive-unmatched/DESK.md`. Desk flash still `needs: squirrel`.
- **M10** AIA MSI: still blocked on CI QEMU 8.2 — see
  `RISCV-AIA-MSI` card above.
- Squirrel-OS/2 boots on every UEFI target once the OS has a port; today
  `os-boot.sh` checks x86. **M6 done:** pin is OS/2 **`v2.3.0`** in
  `tests/os-boot.pin` / `os-boot-sync`; bump the pin when a newer
  release is the supported partner.

### Housekeeping
- Branches ahead of `main` but superseded by merged PRs (check with
  `git log main..BRANCH` before reusing): `int/f3-net` (#143),
  `int/usb-storage-boot` (#133), `ws/bios-legacy/usb-ohci-disk` and
  `ws/bios-legacy/usb-uhci-disk` (#133), `ws/bios-x64/nvram` (#144),
  `ws/newarch/fdt-merge` (#151), `ws/newarch/ppc64-powernv` (#150).
  Do not delete them (Squirrel's call).
