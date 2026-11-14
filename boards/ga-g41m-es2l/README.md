# Gigabyte GA-G41M-ES2L (Intel G41 + ICH7, LGA775, DDR2)

Research and bring-up for ROADMAP C5: one cheap x86 board for the
`x86_64-uefi` firmware. The first step is running as a **coreboot
payload**. Hardware flashing and desk checks stay with the owner
(`needs: squirrel`).

| Piece | Where |
|---|---|
| Flashing guide (SPI programmer, vendor backup/restore, recovery) | [FLASHING.md](FLASHING.md) |
| Payload build | `make PLATFORM=x86_64-uefi CONFIG=coreboot` → `build/x86_64-uefi/coreboot/squirrel-bios.fd` (and `…/obj/body.elf` for coreboot's ELF payload) |
| QEMU stand-in | `make test` case `x86_64-coreboot` (synthetic LBIO tables from `tests/mkcbtables.py`) |

## Why this board

- **Cheap and easy to find.** It is a common second-hand micro-ATX
  board. Any LGA775 Core 2 CPU and two DDR2 DIMMs bring it up.
- **Open silicon init exists.** The board is supported upstream in
  coreboot, and coreboot's G41/x4x memory init was written from scratch
  without a vendor binary. No Intel FSP or other closed blob is needed
  for RAM, and ICH7 runs without a Management Engine firmware region.
  Few x86 boards allow that, and it is what makes the "DDR training is
  research" line of C5 something we can actually study.
- **The chipset is close to QEMU's q35.** QEMU `-M q35` models an ICH9
  and a 3 Series (Q35) MCH. The G41 is the 4 Series MCH that follows
  it, and ICH7 is two generations older than ICH9. `x86_64/platform/
  chipset.c` already knows the register layout this family shares (LPC
  PMBASE/ACPI control, PAM shadow registers, PCIEXBAR), so most of the
  q35 code path should carry over (*verify* each offset in the datasheets).
- **It is easy to recover.** Gigabyte DualBIOS puts a second flash chip
  on the board, and flashrom can write the main chip from Linux.

## The board

Taken from coreboot's and Libreboot's notes on it and from Gigabyte's
manual (*verify* each line on the board in hand, since revisions differ):

| What | Detail | Ours today |
|---|---|---|
| CPU | LGA775 Core 2 Duo/Quad, Pentium Dual-Core, Celeron (64-bit capable) | long mode, CPUID topology |
| Northbridge | Intel G41 (x4x family): DDR2 controller, PCIe x16, integrated GMA X4500 | q35 MCH registers (PAM at 0x90, PCIEXBAR) |
| Southbridge | Intel ICH7: LPC, SATA (IDE / no AHCI on plain ICH7), UHCI + EHCI, HDA, SPI | ICH9 PM block, AHCI, UHCI/EHCI drivers |
| Super I/O | ITE IT8718F: COM1, PS/2, hardware monitor, GPIO | assumes COM1 at 0x3F8 is already on |
| RAM | 2 DIMM slots, DDR2-667/800, up to 8 GB; raminit is picky about mixed or asymmetric DIMMs | QEMU hands over working RAM; payload takes the map from coreboot |
| Flash | 2 x SST25VF080B, 8 Mbit (1 MB) SPI each: main and backup (DualBIOS); SOIC-8 on most boards, a DIP-8 socket reported on some | 1 MB image, QEMU pflash CFI driver |
| Video | GMA X4500 (VGA, no GOP without native init or its VBIOS) | Bochs DISPI GOP only |

The 1 MB flash is the same size as `squirrel-bios.fd`, but it also has to
hold the variable store. The image would need a layout with a descriptor-less
ICH7 flash, the store, then the firmware, with the reset vector at the top.
As a coreboot payload the flash layout is coreboot's; our image is the
payload ELF (or a raw blob coreboot loads), not the full 1 MB `.fd`.

## CONFIG=coreboot

`configs/x86_64-uefi/coreboot.cfg` builds the firmware to consume
coreboot's tables instead of QEMU fw_cfg:

- **Memory map** from the LBIO `CB_TAG_MEMORY` record
  (`x86_64/platform/coreboot.c`, then `memmap.c`).
- **ACPI** from `CB_TAG_ACPI_RSDP` when present: that RSDP is published
  as the EFI configuration table and we do not rebuild FACP/MADT/….
- **Serial** from `CB_TAG_SERIAL` (logged; COM1 stays `CONFIG_COM1_BASE`
  until the IT8718F path is taught to take the table's base).
- **No Secure Boot keys from fw_cfg** (`CONFIG_SECURE_BOOT=n`). Booting
  goes through the ESP on SATA/USB, or the CSM, as on QEMU without a
  `boot.efi` fw_cfg file.

Under QEMU, `tests/mkcbtables.py` builds a stand-in LBIO blob; smoke
loads it at `0x7f000` and expects `mem: 95 MB usable (from coreboot
tables)` and `acpi: … (from coreboot tables)`.

On the board, build coreboot for `gigabyte/ga-g41m-es2l` with this
firmware as the ELF payload (`body.elf` linked at `0x200000`), flash per
[FLASHING.md](FLASHING.md), and confirm the same serial lines. That desk
check is `needs: squirrel`.

The table ABI is the one coreboot documents for payloads ("ABI data
consumption" on doc.coreboot.org): signature `LBIO`, header and table
checksums, tagged records. Our parser is original code written from that
description.

## The way there

### Milestone 1: our UEFI core as a coreboot payload *(CONFIG done; board flash `needs: squirrel`)*

coreboot does what QEMU does for us today: CPU microcode, cache-as-RAM,
DDR2 training, the chipset and the shadow and PCIe setup. It then loads a
payload in 32-bit protected mode with RAM ready, and leaves a table of
its results (the coreboot tables: memory map, serial port, framebuffer
if native graphics init ran, ACPI and SMBIOS if coreboot made them).

Our side:

- **[x] A memory map from the coreboot tables.** `platform/coreboot.c`
  finds the `LBIO` header (and `CB_TAG_FORWARD`), then the memory record.
- **[x] Tables.** ACPI via `CB_TAG_ACPI_RSDP` when coreboot published one;
  otherwise we still build our own (QEMU path). SMBIOS stays ours until
  a published hand-off tag exists for it.
- **[x] No fw_cfg required.** `fwcfg_find` callers already tolerate a
  missing device; the coreboot config turns Secure Boot off so a missing
  db is not a hard failure.
- **[ ] An entry point for the payload.** Today `x86_64/reset/reset.asm`
  starts at the reset vector, copies the body and enters long mode. A
  dedicated 32-bit payload entry (coreboot's ELF jump) that skips the
  flash copy is still open; QEMU tests the table path through `-bios`
  plus a loaded LBIO blob.
- **[ ] The console.** The IT8718F has to be told to turn COM1 on (its
  config ports 0x2E/0x2F) unless coreboot already did.
- **[ ] Graphics.** With coreboot's native GMA init and a linear
  framebuffer, the GOP becomes a framebuffer GOP. `core/gop.c` is
  Bochs-only today.
- **[ ] Variables.** `core/variables.c` drives a CFI parallel flash (QEMU
  pflash). On the board the store is in SPI flash behind ICH7's SPI
  controller (RCBA + 0x3020), which needs a second backend.

### Milestone 2: our own reset vector (research, never a blocker)

This means replacing coreboot below the payload, roughly in boot order:

- CPU microcode loading for the LGA775 steppings in use.
- Cache-as-RAM, which gives a stack before DRAM works.
- ICH7 early init: RCBA, LPC decode, SPI prefetch, the TCO watchdog
  turned off.
- G41 early init: MCHBAR, DMI, the clock settings.
- DDR2 training: SPD over SMBus (ICH7's SMBus controller), timing
  selection, then the training steps the G41 datasheet describes.

This is the part C5 calls research. Reading the G41 and ICH7
datasheets and the JEDEC DDR2 SPD specification is fine. coreboot's
raminit is not a source to copy from (original code only), though its
existence shows the job can be done without blobs.

## Open questions

- Which board revision (1.0, 1.1, 1.2) differs in what (flash package,
  Super I/O)? coreboot documents rev 1.1.
- Should the variable store live in the main chip only, given that
  DualBIOS recovery overwrites the main chip from the backup?
- Is the 1 MB chip enough for coreboot, our payload, the CSM's legacy
  BIOS and a store? Today's `squirrel-bios.fd` is a 1 MB image, but
  only about 200 KB of it is code and data (the body is about 350 KB
  with its zero-filled parts). Next to coreboot's own stages, a
  payload build without the CSM and with a 64 KB store probably fits
  (*verify* with a real coreboot build for the board).

## Sources

Read and summarised in our own words:

- coreboot's mainboard page for the GA-G41M-ES2L and "ABI data
  consumption" (doc.coreboot.org)
- Libreboot's and Canoeboot's hardware and install pages for it
- Gigabyte's GA-G41M-ES2L user manual
- the Intel G41 / 4 Series and ICH7 datasheets (to verify the register
  offsets above)
- SST25VF080B datasheet; flashrom programmer documentation
