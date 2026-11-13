# Roadmap

This file is where SquirrelBIOS is going. [TODO.md](TODO.md) is the list of
changes that get there, one PR each. What the firmware guarantees today is
[common/handoff.md](common/handoff.md). If the firmware and that file
disagree, the firmware has a bug.

SquirrelBIOS is original firmware for Squirrel-OS/2 and for the QEMU
machines this repo builds. **v0.3.0** closed the hand-off era: early init,
load an image, jump, with a tested contract on x86 PC, AArch64 `virt`, and
RISC-V `virt`. **v1.0.0** is a comprehensive **x86 PC BIOS** on QEMU —
runtime services an OS and boot sector actually call — not a joke thin
stub and not a copy of SeaBIOS/EDK2.

After v1.0.0 the direction widens: **one modular, original source tree**
that builds firmware for ancient and modern machines, across the CPU
families mainstream OSes run on, with storage and I/O from floppy and IDE
to NVMe and USB, and with **Squirrel-OS/2 as the OS on top**. That plan is
[Beyond v1.0.0](#beyond-v100-v11--v40) below (epics #82 and #83,
follow-ups #84).

Nothing in this repo is vendored third-party firmware. A UEFI-class
interface arrives in Phase C as original code; until then nothing here is
UEFI.

## Where things are (2026-11-13)

- **Latest release: v1.4.4** (published 2026-11-04, tag on `3388eba`,
  110 `make test` cases). Phases A and B are finished. The table in
  [What works today](#what-works-today-v100) is the v1.0.0 baseline; the
  release notes in `release-notes/` list what each version added.
- **v2.1.0 is prepared** (`release-notes/v2.1.0.md`, 217 `make test`
  cases): C1 to C4 are finished (`x86_64-uefi` with ACPI 6.x, SMBIOS 3.x,
  CSM, SMP and Secure Boot), D1 and D3 are finished (`aarch64-uefi`,
  `riscv64-uefi` from the shared core in `common/efi/`), and F1/F2 have
  their x86-64 parts (NUMA, HMAT, extra PCI roots, MCE/BERT/HEST).
- **Next:** C5 and D2 (real boards, research under `boards/`), D4 (shared
  drivers), Phase E (POWER, SPARC and others), the rest of F.
- Release images are attached to GitHub Releases only. Squirrel
  publishes each release.

## Goals

### Done (v0.3.0 — hand-off)

1. Hand the machine to the OS after early init on QEMU PC, AArch64
   `virt`, and RISC-V `virt`.
2. Every guarantee in the hand-off contract is something `make test` can
   catch if it breaks.
3. x86 loads real images (`0xAA55`, `SQBL`, `S2OK`) and serves enough
   `INT 13h` / `INT 10h` / `INT 15h` E820 for Squirrel-OS/2's boot sector.
4. AArch64 enters a Linux `Image` per the arm64 boot protocol (with
   initrd in the device tree when QEMU provides one).
5. RISC-V provides an original SBI so Linux boots (including a large
   node: many harts, NUMA, AIA aplic, PCIe smoke, RAS shutdown).

### v1.0.0 — comprehensive x86 PC BIOS

On QEMU's PC machine (`qemu-system-i386` / `x86_64` as needed), the
firmware is a real BIOS the OS keeps calling after boot:

1. **POST-shaped init.** Program the PIC and PIT, open A20, fill the
   BIOS data area, leave keyboard and timer interrupts usable.
2. **Runtime interrupt services.** Grow the IVT beyond today's three
   vectors: keyboard (`INT 16h`), time (`INT 1Ah`), richer video text
   (`INT 10h`), disk writes and more disk functions (`INT 13h`), and the
   usual `INT 15h` helpers (A20, etc.) on top of E820.
3. **System tables.** Build ACPI (and SMBIOS) tables the OS can find,
   without making UEFI a requirement.
4. **Boot paths.** Keep Squirrel-OS/2 and `SQBL`; add Linux `bzImage`
   load; scan option ROMs; try more than the primary master when
   present.
5. **Still original, still tested.** Every new service lands with a
   `make test` stand-in and a contract update. Do not vendor SeaBIOS,
   EDK2, or another BIOS.

AArch64 and RISC-V stay supported platform firmware (Linux hand-off and
SBI). Their extra polish lives on a secondary track and does not block
x86 v1.0.0.

## Principles

- **Original code only.** New firmware is written in this repo. Do not
  import or vendor OpenSBI, U-Boot, SeaBIOS, EDK2, or another firmware.
- **Small steps.** One item in [TODO.md](TODO.md) is one PR. The PR
  includes the `make test` check that proves it.
- **Docs move with the code.** The same PR updates `common/handoff.md`,
  and the architecture README when the visible behaviour changes. Check
  the item's box in `TODO.md` in that PR.
- **Branch from `main`.** Every PR targets `main`. Stacked PRs are not
  used. Merge, then start the next item from the new `main`.
- **QEMU is the reference machine.** Addresses, CMOS, fw_cfg, and the
  UARTs are the ones the QEMU machines already use. Every new platform
  runs under QEMU in `make test` before any real board. Real boards live
  under `boards/`, and real-hardware checks are post-merge, never a merge
  blocker.
- **Failure is a message and a halt.** Match the messages already in the
  contract (`disk: ...`, `boot: ...`, then `boot halted`). Never jump to
  a partial or missing image.
- **x86 keeps serving.** After the jump, x86 firmware services stay
  available through the IVT. That is the point of v1.0.0. RISC-V keeps
  the SBI; AArch64 remains load-then-jump unless a later milestone says
  otherwise.

## What works today (v1.0.0)

| | x86 PC | AArch64 `virt` | RISC-V `virt` |
|---|---|---|---|
| Image | ATA PIO: master or slave; `SQBL` / `S2OK` / `bzImage` (`HdrS`) | `-kernel` via fw_cfg DMA; `Image` header; optional `-initrd` in `/chosen` | QEMU loads `-kernel`; S-mode entry when `next_mode` says so |
| Entry | `0x7C00`, `0x8000` (`S2OK`), or PM `code32_start` (`bzImage`). `DL = 0x80`/`0x81` | Image entry (often `0x40200000`), `x0` = DTB, `x1`–`x3` = 0 | `next_addr`, S-mode, `a0` = hart, `a1` = DTB |
| Console | COM1 `0x3F8` 115200 8N1; VGA 80x25 text | PL011 `0x09000000` | NS16550A `0x10000000` 115200 8N1 |
| Memory | SQMM map at `0x0500` from CMOS; `INT 15h` E820 | DTB `/memory` | DTB `/memory`; firmware `no-map` reserved |
| Firmware services | `INT 08h`/`09h`/`10h`/`13h`/`15h`/`16h`/`1Ah`; ACPI; SMBIOS; option ROMs | None after jump | SBI: base, TIME, IPI, RFENCE, HSM, SRST |
| Other CPUs | Boot CPU only | Boot CPU only | Boot hart (+ `sbi_hart_start`); up to 512 harts tracked |

`make test` is the CI gate (58 cases at v1.0.0). `make linux-x86`,
`make linux-shell` /
`make linux-big` are optional RISC-V Linux checks.

## Milestones (done through v1.0.0)

Hand-off milestones 1–7 and x86 PC BIOS milestones 8–12 are finished
and stay checked in [TODO.md](TODO.md) as history.

### 8. x86 foundations (POST and A20)

Fill the BIOS data area the way a PC BIOS does for the fields boot code
reads, enable the A20 line, and program the PIT and PIC so timer and
keyboard IRQs can be used once their handlers exist. Without this, later
services fight QEMU's reset defaults.

### 9. x86 human interface and time

`INT 16h` keyboard (at least check/read keystroke) and `INT 1Ah` time of
day / RTC, with IRQ handlers that keep the BDA counters honest. Boot
sectors and DOS-alike code expect these.

### 10. x86 richer disk and video

Grow `INT 13h` (writes, reset edge cases, primary slave when the master
is absent) and `INT 10h` (cursor, scroll, mode query/set for text — not
a full VESA graphics BIOS). Extend `INT 15h` for A20 control alongside
E820.

### 11. x86 system tables and high loaders

ACPI tables the OS can find (RSDP checksum in a stand-in). SMBIOS where
it helps inventory. Linux `bzImage` load once A20 is on. Option ROM
scan in the legacy regions. These are what make the firmware look like a
real PC to modern and classic loaders.

### 12. v1.0.0 release

When milestones 8–11's TODO boxes are checked: release notes, tag
`v1.0.0`, and a contract that lists the full IVT surface. No SeaBIOS
inside.

## Beyond v1.0.0 (v1.1 → v4.0)

Imported from the SquirrelSoft addendum roadmap (2026-11-04). Epics:
#82 (modular, multi-platform source) and #83 (storage and I/O range).
v1.0.0 follow-ups are #84. Phases run in order; inside a phase, items run
top to bottom. Each item is one PR (or one PR per driver where noted),
with its `make test` case and contract update, exactly as in the
Principles above. Squirrel-OS/2 booting on a target is the primary
acceptance test; Linux and other OS loaders are secondary.

Every item below is a checkbox: tick it (`- [x]`) in the PR that merges
its last piece, and tick the same line on #82 (all phases; #83 was phase B
and is closed). Nested boxes are the item's PR-sized steps; they match the
boxes in [TODO.md](TODO.md).

### Phase A — v1.1: make the tree modular (no behaviour change)

- [x] **A1 (v1.1.0) Build system and configs.** `make PLATFORM=<p>
  CONFIG=<c>`; `configs/<platform>/<name>.cfg` (key=value) generates
  `build/<p>/config.inc` / `config.h`. Platforms: `x86-pc` (today's ROM),
  `aarch64-virt`, `riscv64-virt`. Default `make` builds all three exactly
  as today. Test: `make test` count unchanged, and the `x86-pc/default`
  ROM is byte-for-byte v1.0.0's ROM (or a documented, intentional diff).
- [x] **A2 (v1.1.1) Split x86 into modules.** `x86/core/` (reset, POST,
  IVT), `drivers/ata/`, `drivers/vga/`, `drivers/kbd8042/`,
  `drivers/rtc/`, `tables/acpi/`, `tables/smbios/`, `boot/`, each
  guarded by `CONFIG_*`. Test: a `CONFIG=minimal` ROM boots the
  `x86-boot` test.
- [x] **A3 (v1.1.2) v1.0.0 follow-ups (#84).** bzImage loader for kernels
  over 64 KB; `boot_params` gets the E820 table, command line and
  `type_of_loader`; slave detection uses fresh status reads; `INT 13h
  AH=08h` fails for absent drives. Test: an `x86-slave-absent` case and a
  >64 KB synthetic bzImage; an optional job boots a real Linux bzImage.

### Phase B — v1.2–v1.4: storage and I/O on x86 legacy (#83)

- [x] **B1 (v1.2.0) Floppy.** 82077 FDC driver, `INT 13h` `DL=00h/01h`
  read/write, `INT 1Eh` diskette parameter table, boot from `-fda`.
  Test: boot a 1.44 MB Squirrel-OS/2 `floppy.img`; read/write/verify.
- [x] **B2 (v1.2.1) PCI enumeration.** Config mechanism #1, bus scan, BAR
  sizing, `$PIR` routing table, PCI BIOS `INT 1Ah AX=B1xx`. Test:
  enumerate QEMU `i440fx` and `q35`; `B101h` returns `"PCI "`.
- [x] **B3 (v1.2.2) EDD 3.0 and LBA48.** `INT 13h AH=41h/42h/43h/48h`, disks
  over 8 GB, El Torito no-emulation boot over ATAPI. Test: a 16 GB disk;
  an ISO with `-cdrom`.
- [x] **B4 (v1.3.0) AHCI/SATA.** HBA init, port reset, READ/WRITE DMA EXT,
  `INT 13h` on top. Test: `-machine q35` AHCI disk boots Squirrel-OS/2.
- [x] **B5 (v1.3.1) NVMe.** Admin and one I/O queue pair, Identify,
  Read/Write, `INT 13h` on top. Test: `-device nvme` boots.
- [x] **B6 (v1.4.0) USB host controllers.** UHCI, OHCI, EHCI, xHCI (in that
  order, one PR each), enumeration, hubs.
- [x] **B7 (v1.4.1) USB HID with legacy emulation.** Keyboard feeds
  `INT 16h` and the BDA buffer; mouse feeds `INT 15h C2xx`. Test:
  `-device usb-kbd` with scripted keys.
- [x] **B8 (v1.4.2) USB mass storage boot.** Test: `-device usb-storage`
  with `os.img` boots.
  - [x] Bulk-only transport on xHCI, `INT 13h` as `DL = 80h` (#114)
  - [ ] UAS (follow-up; not a v1.4.4 item)
- [x] **B9 (v1.4.3) virtio-blk / virtio-net and network boot.** Original
  TFTP client; boot `SQBL` over the network. Test: QEMU user-net TFTP.
- [x] **B10 (v1.4.4) Video.** VBE 2.0/3.0 linear-framebuffer modes on QEMU
  std VGA (still not a full SVGA option-ROM replacement). Test: mode
  list, set 800x600x32, read back a pixel.

### Phase C — v2.0: x86-64 and a UEFI-class interface

- [ ] **C1 (v2.0.0-alpha) `PLATFORM=x86_64-uefi`.** Reset to long mode in
  firmware, page tables, PE32+ loader. Original boot services subset an
  OS loader needs (memory map, GOP, Simple Text In/Out, Block I/O, Simple
  File System over FAT32, LoadImage/StartImage, ExitBootServices) and
  runtime services (GetTime, ResetSystem, variables). Test: `-machine
  q35 -bios` boots a test PE32+ app; a Squirrel-OS/2 UEFI loader boots.
  - [x] Step 1 `x86_64-reset`: reset vector to long mode, COM1 banner,
    memory map from fw_cfg (#118)
  - [x] Step 2 `x86_64-boot-services`: EFI system table, boot services
    core, memory services (#119)
  - [x] Step 3 `x86_64-pe`: PE32+ loader, LoadImage/StartImage, a test app
    (#122)
  - [x] Step 4 `x86_64-console`: Simple Text Input/Output, events and
    timers (#123)
  - [x] Step 5 `x86_64-fat`: Block I/O and FAT32 Simple File System, boot
    `\EFI\BOOT\BOOTX64.EFI`
  - [x] Step 6 `x86_64-gop`: GOP, runtime services, ExitBootServices
    (this PR; depends on step 5)
- [x] **C2 (v2.0.0) ACPI 2.0+/6.x and SMBIOS 3.x.** MADT, HPET, MCFG, FADT,
  DSDT (original, generated AML). Test: Linux x86-64 boots to a shell
  with ACPI; an `acpidump`-style self-check in `make test`.
- [x] **C3 (v2.0.1) CSM.** Legacy BIOS services kept on the x86-64 build.
- [x] **C4 (v2.1.x) SMP and Secure Boot.** INIT-SIPI-SIPI, MP table, x2APIC;
  verify-only Secure Boot with our own keys.
  - [x] `c4-smp`: AP bring-up (INIT-SIPI-SIPI), AP parking, MP services
    protocol, MPS table, x2APIC mode; APs parked at ExitBootServices and
    before the CSM
  - [x] `c4-secure-boot`: verify-only Secure Boot with our own keys
    (Authenticode SHA-256, PKCS#7 RSA-2048 signatures, db/dbx)
- [ ] **C5 (v2.2.x) Real x86 hardware track.** A flashing guide for one
  cheap board; DDR training is research under `boards/` and never a
  merge blocker.
  - [ ] `c5-tpm`: measured boot with a TPM 2.0 on TIS (PCR extends, the
    TCG2 protocol and event log, the TPM2 table)

### Phase D — v2.5: ARM and RISC-V as full firmware

- [x] **D1** AArch64 `virt`: UEFI-class interface, device tree and ACPI
  (SBBR-like), PSCI, GICv3.
- [ ] **D2** AArch64 boards: Raspberry Pi 4/5 under `boards/rpi4`.
- [x] **D3** RISC-V `virt`: UEFI-class interface while keeping the SBI;
  `DBCN`, SSE; boards (VisionFive 2, HiFive) under `boards/`.
- [ ] **D4** Shared drivers across architectures (NVMe, xHCI, virtio, FAT,
  PE/ELF loaders) from the same sources. Test: each QEMU target boots
  Squirrel-OS/2 (once its port exists) and Linux to a shell.

### Phase E — v3.0: other architectures

- [ ] **E1** `PLATFORM=ppc64le-powernv`: OPAL-like runtime interface, device
  tree, boot Linux on QEMU `powernv`.
- [ ] **E2** `PLATFORM=sparc64-sun4u`: an IEEE 1275-style client interface
  subset (no Forth), boot on QEMU `sun4u`.
- [ ] **E3** Stretch: `m68k-virt`, `mips-malta`, `loongarch64-virt`, `s390x`
  (IPL only). Test: each reaches a firmware prompt under QEMU and hands
  off to a test kernel.

### Phase F — v4.0: large and supercomputer-class nodes

- [ ] **F1** NUMA (SRAT/SLIT/HMAT), more than 1000 CPUs, many PCIe root
  complexes and ECAM segments.
  - [x] `f1-x86-numa`: x86_64 SRAT (CPU and memory affinity) and SLIT
    from QEMU `-numa`, MADT from the SMP list with x2APIC entries past
    APIC ID 254 (HMAT, more than 1000 CPUs and ECAM segments still open)
  - [x] `f1-x86-hmat`: x86_64 HMAT (initiator of each memory node, or
    QEMU's latency, bandwidth and cache structures) and QEMU's `-numa
    dist` in the SLIT
  - [x] `f1-x86-roots`: x86_64 extra PCI root buses (QEMU pxb and
    pxb-pcie) as DSDT host bridges with their own buses and windows, and
    ACPI table room that grows with the CPU count (more than one ECAM
    segment and >255 CPUs untestable on QEMU x86 TCG)
- [ ] **F2** RAS: machine-check logging (BERT/HEST/EINJ), corrected-error
  thresholds, CXL memory enumeration (research).
  - [x] `f2-x86-stubs`: x86_64 BERT and HEST stubs (MCE logging, EINJ and
    thresholds still open)
  - [x] `f2-x86-mce`: x86_64 machine-check banks read at boot, errors
    from before a reset as CPER records in the BERT, HEST IA-32 MCE and
    corrected machine check sources with CMCI or polling and thresholds
    (EINJ still open)
- [ ] **F3** Management: IPMI/Redfish-style BMC stubs, serial-over-LAN, HTTP
  boot.
  - [x] `f3-uefi-net`: UEFI Simple Network Protocol on virtio-net and a
    PXE-style DHCP+TFTP boot on x86_64, aarch64 and riscv64 UEFI
  - [x] `f3-http-boot`: HTTP boot (own TCP, HTTP/1.1 with Content-Length,
    chunked, close-delimited bodies and redirects; IPv4 hosts, no DNS yet)
- [ ] **F4** Diskless cluster boot of many nodes from one image server.
  Test: QEMU `-smp 256 -numa ...`; `make linux-big` already covers part.

### Squirrel-OS/2 hand-off on every platform

Every platform build provides the same Squirrel hand-off
([common/handoff.md](common/handoff.md)): memory map, console,
framebuffer (if any), ACPI/DTB pointer, boot device. Squirrel-OS/2 boots
on each new platform as soon as its port exists.

## Secondary track

The older polish items stay valid and fold into Phase D when it starts:

- [ ] RISC-V: `DBCN`, device-tree-derived UART/CLINT addresses, SSE once
  QEMU can inject corrected errors, `aplic-imsic` when the hypervisor
  stack allows a shell.
  - [x] `DBCN` (SBI 2.0 debug console; Linux `earlycon=sbi`)
  - [x] UART and CLINT/ACLINT addresses taken from the device tree in the
    M-mode SBI (`CONFIG_UART_BASE`/`CONFIG_CLINT_BASE` as fallbacks)
- [ ] AArch64: firmware PSCI/GIC only if QEMU's built-ins are disabled for a
  test; otherwise leave them.

## Out of scope

- Vendoring SeaBIOS, EDK2, OpenSBI, U-Boot, coreboot, OpenBIOS, or
  another project's firmware and calling it SquirrelBIOS. Reading public
  specifications is fine; cite them.
- A full SVGA option-ROM replacement (VBE linear-framebuffer modes in B10
  are in scope).
- Committing firmware or OS images. Release images go only on GitHub
  Releases, and only Squirrel tags or publishes. The "Draft release"
  workflow (Actions tab, run by Squirrel) builds and tests a commit and
  leaves a draft release with the images and `release-notes/v<version>.md`;
  publishing the draft creates the tag.

UEFI-class interfaces and boards other than the QEMU machines used to be
listed here. Epic #82 moves both in scope, staged as Phases C and D.
