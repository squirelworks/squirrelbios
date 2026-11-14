# Hand-off to Cursor / Codex (2026-11-14)

From 2026-11-14 the remaining SquirrelBIOS work is left for Cursor or Codex to
pick up. This file says what is done, what is in flight, and what is open,
with enough detail to start any open item cold. The suite-wide plan for what
comes after this list is
[squirrelsoft `docs/CONTINUANCE.md`](https://github.com/squirelworks/squirrelsoft/blob/main/docs/CONTINUANCE.md).

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

- **Released:** v2.1.1 (2026-11-13; v2.1.0's firmware from `f572f71` with all
  six images attached).
- **`main` is ready for v2.2.0** (`4a3d29f`, including F2-CXL + F3-DNS #167):
  native AHCI / NVMe / virtio disks and AML on the x86_64 UEFI core (D4,
  #142), FAT write and USB mass storage in the shared UEFI core (#145),
  UEFI network boot (SNP, DHCP+TFTP, HTTP boot with DNS) and serial-over-LAN
  on all three UEFI targets (#143, #146, #167), multi-node diskless boot
  (F4, #146), non-volatile and authenticated variables (C5, #144), TPM 2.0
  measured boot (#147), SMM, EINJ and CXL CEDT (F2, #148, #149, #167),
  IPMI KCS / SPMI / Redfish host interface stub (F3, #154), legacy El Torito
  hard-disk emulation and VBE 4F0Ah (#141), AArch64 EL3 + PSCI, SDHCI and
  Raspberry Pi bring-up on QEMU `raspi3b` (#157), and all of Phase E:
  powernv (#150, #158), sun4u (#153, #159, #161), m68k, mips-malta,
  loongarch64 (#155, #159), s390x (#160, #161). Licence is GPL-3.0 (#163).
  Release notes are in `release-notes/v2.2.0.md`; publishing is `needs: squirrel`.
- **Test gate:** `make test` (334 cases in `tests/smoke.sh`), run by
  `.github/workflows/make-test.yml` on every PR. Optional, not in CI:
  `make linux-x86`, `make linux-uefi`, `make linux-shell`, `make linux-big`.
- **Open PRs at hand-off:** none for the condense work; F2-CXL + F3-DNS are on `main`.

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

1. **REL-2.2.0** *(Squirrel)*: `Actions → Draft release` for v2.2.0 on the
   merge commit, then publish.
2. **D2-rpi4** on QEMU `raspi4b` (QEMU ≥ 9.0), then the real Pi as a post-merge note.
3. **F1-scale** (table generation for >1000 CPUs and several ECAM segments,
   checked by a host-side parser).
4. **C5-board** flashing guide (needs Squirrel and a board).
5. **RISC-V AIA MSI** when a newer QEMU is available in CI.
6. Then Phase G ideas in `docs/CONTINUANCE.md` (squirrelsoft).

## Task cards

### REL-2.2.0 — release notes for what `main` has since v2.1.1  `needs: squirrel`
- **Done (notes):** `release-notes/v2.2.0.md` (style of `v2.1.0.md`; Known
  gaps refreshed; UEFI banner `SQ_VERSION` `2.2.0`).
- **Open (Squirrel):** run `Actions → Draft release` on the merge commit and
  publish; give the draft / release link.
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

### D2-rpi4 — Raspberry Pi 4 build on QEMU and on the board  `cursor-codex` (board check `needs: squirrel`)
- **Done:** `configs/aarch64-uefi/rpi3.cfg` and `rpi4.cfg`, SDHCI driver,
  EL3 + PSCI, `rpi3-uefi-sd` case on QEMU `raspi3b` (#157); research in `boards/rpi4/README.md`.
- **Open:** a `rpi4-uefi-*` case on QEMU `raspi4b` (QEMU 9.0+; add a newer QEMU
  to CI or run it as an optional target if the runner's QEMU is older), the
  BCM2711 EMMC2 controller, GIC-400 from the tree, and a "try it on a Pi 4"
  section with the exact SD card layout (`config.txt`, `kernel=`). Pi 5
  (BCM2712, RP1) stays research.
- **Acceptance:** under `raspi4b` the firmware reaches the EFI stand-in
  with `disk: SD card` and `ExitBootServices`, like `rpi3-uefi-sd`.

### F1-scale — more than 1000 CPUs and several ECAM segments  `cursor-codex`
- **What:** the x2APIC MADT/SRAT and multi-segment MCFG are built but QEMU x86
  TCG cannot run them. Make them testable: a build-time or host-side test that
  feeds a synthetic CPU/NUMA/PCI topology into the table builders and checks
  the tables with a small Python ACPI parser (`tests/acpicheck.py`).
- **Files:** `x86_64/` table builders, new `tests/acpicheck.py`, Makefile rule.
- **Acceptance:** `x86_64-tables-1024cpu` and `x86_64-tables-2seg` cases: the
  parser checks every checksum, 1024 x2APIC entries with distinct IDs, SRAT
  affinity per node, and two MCFG allocations with their own bus ranges.
  Optional: a KVM run on a big host as a post-merge note.

### C5-board — real x86 hardware  `needs: squirrel` (docs part `cursor-codex`)
- **Research:** `boards/ga-g41m-es2l/README.md` (Intel G41 + ICH7; first as a coreboot payload).
- **Open:** a flashing guide (external SPI programmer, backup and restore of
  the vendor image, recovery), and a `CONFIG=` for running as a coreboot
  payload (take memory map and tables from coreboot's tables instead of
  fw_cfg). DDR training stays research. Hardware checks are Squirrel's.

### RISCV-AIA-MSI — Linux on `virt,aia=aplic-imsic`  `cursor-codex` (blocked on QEMU)
- **Background:** TODO.md `riscv-aia`: APLIC direct mode works; MSI mode storms on QEMU 8.2.
- **Open:** program `smsiaddrcfg`/`smsiaddrcfgh` in the M-level APLIC, retest with
  QEMU ≥ 9.1 and a 6.14+ kernel; `LINUX_AIA=imsic make linux-big` reaches the shell.

### Secondary track and small follow-ups  `cursor-codex`
- `f4-scale`: the multi-node netboot case with more nodes and a `-smp 256 -numa` node, as an optional `make` target if it is too slow for CI.
- `riscv-sparse-harts` only if a board with gapped hart IDs is targeted (Speculative in TODO.md).
- VisionFive 2 and HiFive Unmatched: research notes exist in `boards/`; first
  code step is a `CONFIG=` per board with the UART and memory from their trees.
- Squirrel-OS/2 boots on every UEFI target once the OS has a port; today
  `os-boot.sh` checks x86. Keep `tests/os-boot.sh` in step with squirrel-os2 releases.

### Housekeeping
- Branches ahead of `main` but superseded by merged PRs (check with
  `git log main..BRANCH` before reusing): `int/f3-net` (#143),
  `int/usb-storage-boot` (#133), `ws/bios-legacy/usb-ohci-disk` and
  `ws/bios-legacy/usb-uhci-disk` (#133), `ws/bios-x64/nvram` (#144),
  `ws/newarch/fdt-merge` (#151), `ws/newarch/ppc64-powernv` (#150).
  Do not delete them (Squirrel's call).
