# Agent guide (Cursor, Codex and other coding agents)

SquirrelBIOS: original firmware (x86 PC BIOS, x86-64/AArch64/RISC-V UEFI-class, POWER, SPARC,
m68k, MIPS, LoongArch, s390x) tested under QEMU.

- **Start here:** [HANDOFF.md](HANDOFF.md) (done / in progress / open, with a card per open item),
  then [ROADMAP.md](ROADMAP.md), [TODO.md](TODO.md) and [common/handoff.md](common/handoff.md).
  Suite-wide plan: `docs/CONTINUANCE.md` in github.com/squirelworks/squirrelsoft.
- **Build:** `make` (or `make PLATFORM=<p> CONFIG=<c>`). **Test:** `make test` (QEMU smoke cases in
  `tests/smoke.sh`; packages as in `.github/workflows/make-test.yml`). CI runs it on every PR.
- **PRs:** base on `main`; never stack on another feature branch; group related steps into one
  sizeable PR; confirm the PR targets `main`, is mergeable and `make-test` is green.
- **Original code only.** Never vendor or copy SeaBIOS, EDK2/OVMF, OpenSBI, U-Boot, coreboot,
  OpenBIOS, SLOF or Linux code. Public specs are fine; cite them.
- Every behaviour change: a `make test` case, `common/handoff.md` / platform README updated, the box
  ticked in `ROADMAP.md`/`TODO.md` and on epic #82. Failures print a message and `boot halted`.
- Never commit firmware or OS images, never tag or publish releases, never delete branches, never
  merge `klingon-font*` branches. Release notes go in `release-notes/`; the owner publishes.
