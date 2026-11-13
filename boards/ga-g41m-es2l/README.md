# Gigabyte GA-G41M-ES2L (Intel G41 + ICH7, LGA775, DDR2)

Research for ROADMAP C5: one cheap x86 board for the `x86_64-uefi`
firmware, with a flashing guide. It is research: nothing here is built,
and nothing in `make test` depends on it.

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
| RAM | 2 DIMM slots, DDR2-667/800, up to 8 GB; raminit is picky about mixed or asymmetric DIMMs | QEMU hands over working RAM |
| Flash | 2 x SST25VF080B, 8 Mbit (1 MB) SPI each: main and backup (DualBIOS); SOIC-8 on most boards, a DIP-8 socket reported on some | 1 MB image, QEMU pflash CFI driver |
| Video | GMA X4500 (VGA, no GOP without native init or its VBIOS) | Bochs DISPI GOP only |

The 1 MB flash is the same size as `squirrel-bios.fd`, but it also has to
hold the variable store. The image would need a layout with a descriptor-less
ICH7 flash, the store, then the firmware, with the reset vector at the top.

## Flashing guide

Read the whole chip before writing anything, and keep the dump off the board.

1. **Get a console first.** Without one, a board that does not boot
   tells you nothing. The IT8718F's COM1 goes to a 10-pin header on the
   board (*verify* its location in the manual) and needs a header-to-DB9
   bracket and a null-modem cable, or a 3.3 V USB serial adapter on the
   header's TX/RX/GND. coreboot also supports an EHCI debug dongle on
   this board.
2. **Back up both chips with the vendor BIOS still running.** Boot any
   Linux on the board and run
   `flashrom -p internal -r vendor-main.rom` twice, then compare the two
   reads. flashrom's `internal` programmer has a `dualbiosindex=` option
   for Gigabyte boards. coreboot's page for this board writes with
   `-p internal:dualbiosindex=0`, the main chip. Read the backup chip
   with `dualbiosindex=1` as well (*verify* that index 1 is the backup on
   this board's flashrom version).
3. **Write the main chip only:**
   `flashrom -p internal:dualbiosindex=0 -w image.rom`, then the same
   command with `-v image.rom` to verify. Leave the backup chip alone so
   the vendor BIOS stays on it.
4. **Recovering.**
   - Internal: if the main chip boots nothing, DualBIOS may copy the
     backup over it. That gets the vendor firmware back, but the image you
     wrote is lost (*verify* what triggers the copy and whether it asks first).
   - External: use a SOIC-8 clip (or pull the DIP-8 chip on boards that
     have a socket) with a CH341A-class programmer or a Raspberry Pi
     (`flashrom -p linux_spi`).
   - Libreboot warns that the flash shares its 3.3 V rail with the
     southbridge, so powering it from the programmer's 3.3 V under-volts
     it. Power the board's own standby rail instead, or desolder the chip.

## The way there

### Milestone 1: our UEFI core as a coreboot payload

coreboot does what QEMU does for us today: CPU microcode, cache-as-RAM,
DDR2 training, the chipset and the shadow and PCIe setup. It then loads a
payload in 32-bit protected mode with RAM ready, and leaves a table of
its results (the coreboot tables: memory map, serial port, framebuffer
if native graphics init ran, ACPI and SMBIOS if coreboot made them).

Our side needs:

- **An entry point for that.** Today `x86_64/reset/reset.asm` starts at
  the reset vector, builds page tables at 0x1000 and enters long mode.
  A payload entry would take the 32-bit hand-off and do the same from
  there, at an address that is free in coreboot's memory map.
- **A memory map from the coreboot tables.** `platform/memmap.c` reads
  QEMU's `etc/e820` or the CMOS sizes. A coreboot-table reader (the
  `LBIO` header, then the memory record) would give the ranges,
  including the ones coreboot keeps for itself.
- **No fw_cfg.** Every `fwcfg_find` caller (boot image, cmdline, Secure
  Boot keys, NUMA, ACPI from QEMU) must keep working when fw_cfg is
  absent. Most already check for that. Booting goes through the ESP path
  (`efi_boot_esp`) on SATA or USB instead.
- **Tables.** One owner per table. Either we keep making the ACPI tables
  (`core/acpi.c` assumes the q35 or PIIX4 layout: ICH7's GPE0 block,
  PCI interrupt routing through the ICH7 PIRQ registers, and the HPET
  base from RCBA all differ, *verify*), or coreboot's tables are passed
  through as they are.
- **The console.** The IT8718F has to be told to turn COM1 on (its
  config ports 0x2E/0x2F) unless coreboot already did. The coreboot
  table gives the base address and speed.
- **Graphics.** With coreboot's native GMA init and a linear
  framebuffer, the GOP becomes a framebuffer GOP: no mode switching,
  just `Blt` into the buffer the table describes. `core/gop.c` is
  Bochs-only today.
- **Variables.** `core/variables.c` drives a CFI parallel flash (QEMU
  pflash). On the board the store is in SPI flash behind ICH7's SPI
  controller (RCBA + 0x3020: address, data, opcode menu, software
  sequencing), which needs a second backend. Writes must go through the
  controller at run time too, so its registers need a runtime MMIO entry
  in the memory map like the pflash range has now.

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
- coreboot's mainboard page for the GA-G41M-ES2L (doc.coreboot.org)
- Libreboot's and Canoeboot's hardware and install pages for it
- Gigabyte's GA-G41M-ES2L user manual
- the Intel G41 / 4 Series and ICH7 datasheets (to verify the register
  offsets above)
