# Klingon ASCII purge (joke branch only)

**Branch:** `klingon-font` only (a [core branch](CORE_BRANCHES.md) beside
`main`). Never merge this plan or its eventual implementation into `main`.
Refresh by merging `main` *into* this branch.

## End state

Every human-readable string the firmware emits — VGA **and** COM1 — is
Klingon. No Latin letters in banners, disk errors, USB lines, UEFI ConOut
paths that this branch owns, or smoke expects. Connecting over serial means
you read Hol / pIqaD (or you are not in on the joke).

Breaking COM1 Latin is intentional. Call it cybersecurity theatre: casual
sniffers and Latin-expecting scripts see noise; conspirators with a pIqaD
font (or a Hol decoder) still boot fine.

## Current state (2026-11-15)

| Path | Behaviour |
|------|-----------|
| VGA | `klingon-font.inc` glyphs; digraph fold `ch`/`gh`/`ng`/`tlh` → `0x80`–`0x83` in `core/console.inc` |
| COM1 | Still Latin ASCII (same bytes `serial_putc` gets before/without fold) |
| Strings in ROM | Latin source (`banner`, `msg_*`, …) plus `banner_tlh` |
| `tests/smoke.sh` | Expects Latin needles (`SquirrelBIOS x86`, `boot halted`, …) |

## Target state

1. **COM1 = VGA policy.** Digraph fold (or Hol emit) applies to serial too,
   or strings are authored as Hol so both paths print the same code points.
2. **No Latin `db` messages** in x86 (and, when this branch carries them,
   UEFI ConOut banners). Digits and punctuation may stay ASCII if Hol has
   no better glyph; letters must not.
3. **Smoke is Klingon.** `tests/smoke.sh` expect strings on this branch are
   Hol / pIqaD bytes (or a branch-local expect table). `main` keeps Latin.
4. **Docs / README** on this branch describe the joke in English only as
   meta; runtime output is never Latin.

## Scaffolding checklist (do later, on this branch)

When ready to flip the switch, work these in order. Each item is one
logical change; keep the branch mergeable from `main` between steps.

### A. Policy + plumbing

- [ ] Document the chosen wire encoding (pick one and stick to it):
  - **A1 — font codes on the wire:** VGA digraph codes `0x80`–`0x83` also
    go out COM1; clients need the same 8-bit font map (or accept mojibake
    until they load one).
  - **A2 — Hol Latinization:** message text is romanized Hol (`tlhIngan…`)
    on both paths; VGA still maps through `klingon-font.inc`.
  - **A3 — UTF-8 pIqaD:** serial emits Unicode pIqaD; VGA keeps the 8×16
    ROM font. Dual encode at `con_putc` / `serial_putc`.
- [ ] Extend digraph fold (or Hol emit) so COM1 is not a Latin bypass.
  Today `con_putc` folds only `es:[di]`; serial gets the raw `AL` first.
- [ ] Gate with a single asm `%define` / config bit if a temporary dual
  mode helps (`KLINGON_COM1=1`), then delete the Latin path.

### B. String inventory (x86 first)

Replace or rewrite every user-visible `db` / ConOut string. Start here:

- [ ] `x86/squirrel-bios.asm` — `banner`, `banner_tlh`, all `msg_*`
- [ ] `x86/core/entry.inc` / `fail.inc` / boot paths that print
- [ ] `x86/drivers/**` — USB, AHCI, NVMe, virtio, PCI, disk lines
- [ ] `x86/boot/**` — floppy / El Torito / netboot / bzImage lines
- [ ] Optional later: `x86_64/` and `common/efi` banners if this branch
  still builds those images with Latin POST text

Keep a living list while grepping:

```bash
rg -n "db ['\"]" x86/ --glob '*.asm' --glob '*.inc'
rg -n 'con_print|serial_puts|msg_' x86/
```

### C. Tests (this branch only)

- [ ] Fork or branch-local expect table: every `run … "Latin|…"` in
  `tests/smoke.sh` that matches x86 (and any other purged platform) uses
  Klingon needles instead.
- [ ] Do **not** carry those expects onto `main`. On merge-from-`main`,
  re-apply the Klingon expect patch if `smoke.sh` conflicted.
- [ ] Add one smoke case that fails if a Latin letter `[A-Za-z]` appears
  in the x86 serial log after POST banner (purge guard).

### D. Tooling / font

- [ ] `tools/klingon-font.py` — keep regenerating `x86/klingon-font.inc`
- [ ] If A3: document a host font / terminal profile for reading COM1
- [ ] Refresh `docs/klingon-font.png` after the purge

### E. Done criteria

- [ ] QEMU VGA screendump: no Latin letters in firmware-drawn cells
- [ ] COM1 capture: no Latin letters in firmware lines (per chosen encoding)
- [ ] `make x86` and branch `make test` green with Klingon expects
- [ ] README joke section points here and says COM1 is Klingon by design

## Explicit non-goals

- Merging any of this into `main`
- Tagging a Klingon release (owner only; and this stays a joke branch)
- Requiring real hardware to speak Hol before the checklist above is done

## Refresh reminder

```bash
git fetch origin main
git checkout klingon-font
git merge origin/main
# re-resolve joke files + this doc if main touched README/HANDOFF
make x86 && make test
git push origin klingon-font
```
