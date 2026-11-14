# Flashing the GA-G41M-ES2L

How to back up the vendor BIOS, write a coreboot image that carries
SquirrelBIOS as its payload, and recover if the board no longer boots.
Hardware checks (a real write and a real boot) stay with the owner
(`needs: squirrel`); this guide is the procedure those checks follow.

Read every step before touching the flash. Keep the vendor dumps off the
board and on at least two media.

## What you need

- The board, a known-good LGA775 CPU, two matched DDR2 DIMMs, a PSU, and
  a case or open bench that lets you reach the SPI chips.
- A serial console: a 10-pin COM1 header-to-DB9 bracket (or a 3.3 V USB
  serial adapter on TX/RX/GND of that header). Without a console, a
  board that does not boot tells you nothing.
- Software: Linux on the board for the internal programmer path, and
  [flashrom](https://flashrom.org/) (the same tool coreboot's and
  Libreboot's install notes use for this mainboard).
- For the external path: a SOIC-8 test clip (or a DIP-8 puller on boards
  that socket the chip), and a CH341A-class SPI programmer **or** a
  Raspberry Pi with `spidev` (`flashrom -p linux_spi`).
- Images: two verified dumps of the vendor main and backup chips, the
  coreboot ROM built for this board with SquirrelBIOS as the payload
  (`make PLATFORM=x86_64-uefi CONFIG=coreboot`; see
  [README.md](README.md)), and a way to restore the vendor image.

The flash parts are two SST25VF080B (8 Mbit / 1 MB) SPI chips — main and
backup (Gigabyte DualBIOS). Most boards use SOIC-8; some revisions are
reported with a DIP-8 socket. Confirm the package on the board in hand
against Gigabyte's GA-G41M-ES2L user manual for that revision.

## 1. Get a console first

1. Fit the COM1 bracket or USB serial adapter. The Super I/O is an ITE
   IT8718F; its UART0 is the board's COM1 (I/O `0x3F8` once enabled).
2. On the host: `115200 8N1`, no flow control.
3. Power on with the vendor BIOS still present and confirm you see POST
   or, under Linux, that `/dev/ttyS0` works. Fix cabling before any
   write.

coreboot also supports an EHCI debug dongle on this board; that is an
optional second console, not a substitute for COM1 during the first
bring-up.

## 2. Back up both chips (vendor image still running)

Boot any Linux on the board with the vendor BIOS. Do **not** write yet.

```sh
# Main chip (DualBIOS index 0 on Gigabyte boards flashrom knows)
flashrom -p internal:dualbiosindex=0 -r vendor-main-1.rom
flashrom -p internal:dualbiosindex=0 -r vendor-main-2.rom
cmp vendor-main-1.rom vendor-main-2.rom

# Backup chip (index 1 — verify on your flashrom build that 1 is backup)
flashrom -p internal:dualbiosindex=1 -r vendor-backup-1.rom
flashrom -p internal:dualbiosindex=1 -r vendor-backup-2.rom
cmp vendor-backup-1.rom vendor-backup-2.rom
```

Copy the four files (or the two that compared equal) off the machine.
If the two reads of a chip disagree, stop and re-read until they match;
a flaky read is not a backup.

flashrom's `internal` programmer and the `dualbiosindex=` option for
Gigabyte boards are documented in flashrom's own programmer notes.
coreboot's mainboard page for the GA-G41M-ES2L writes the main chip with
`-p internal:dualbiosindex=0`.

## 3. Write the main chip only

Leave the backup chip alone so DualBIOS still holds the vendor image.

```sh
flashrom -p internal:dualbiosindex=0 -w coreboot-with-squirrel.rom
flashrom -p internal:dualbiosindex=0 -v coreboot-with-squirrel.rom
```

Reboot with the serial console attached. With SquirrelBIOS as the
payload you should see the `SquirrelBIOS` banner and
`mem: … (from coreboot tables)` (the `CONFIG=coreboot` build). If the
console stays silent, go to recovery; do not write the backup chip.

## 4. External SPI programmer (when internal is unavailable)

Use this when Linux will not boot, flashrom's internal programmer
refuses the chip, or DualBIOS recovery is not enough.

1. Power the board off and unplug the AC cord. Identify the **main**
   SPI chip (the one DualBIOS selects first; the board silkscreen and
   Gigabyte's manual label the pair — *verify* on the revision you have).
2. Attach the SOIC-8 clip with pin 1 aligned. For a DIP-8 socket, lift
   the chip and seat it in the programmer instead.
3. **Power:** Libreboot's and Canoeboot's install notes for this board
   warn that the flash shares its 3.3 V rail with the southbridge. Do
   not feed the clip from the programmer's 3.3 V alone while the chip is
   still on the board — that under-volts the rail. Either:
   - power the board's +3.3 V standby from the PSU (plug in AC, soft-off
     so the chip is powered but the CPU is not running), and connect
     only GND / CLK / MOSI / MISO / CS from the programmer, **or**
   - desolder the chip and program it in a socket on the programmer.
4. Read before write:

```sh
flashrom -p ch341a_spi -c SST25VF080B -r ext-main-1.rom
flashrom -p ch341a_spi -c SST25VF080B -r ext-main-2.rom
cmp ext-main-1.rom ext-main-2.rom
```

(or `-p linux_spi:dev=/dev/spidev0.0` on a Pi). Compare with the
internal vendor dump when you have one.

5. Write and verify:

```sh
flashrom -p ch341a_spi -c SST25VF080B -w coreboot-with-squirrel.rom
flashrom -p ch341a_spi -c SST25VF080B -v coreboot-with-squirrel.rom
```

6. Remove the clip, restore any desoldered chip, and power on with the
   serial console attached.

## 5. Restore the vendor image

From Linux, when the board still boots enough for flashrom:

```sh
flashrom -p internal:dualbiosindex=0 -w vendor-main-1.rom
flashrom -p internal:dualbiosindex=0 -v vendor-main-1.rom
```

From the external programmer, the same `-w` / `-v` pair as in section 4
with `vendor-main-1.rom`. Only write the backup chip if that copy is
also damaged, and only with `vendor-backup-1.rom`.

## 6. Recovery when the main chip does not boot

1. **DualBIOS internal recovery.** If the main chip fails POST, DualBIOS
   may copy the backup chip over the main one and bring the vendor BIOS
   back. That restores a bootable vendor image; the image you wrote to
   the main chip is lost. *Verify* on your board revision what triggers
   the copy (power cycles, a jumper, or a prompt) against Gigabyte's
   manual — behaviour differs by BIOS version.
2. **External rewrite.** If DualBIOS does not recover, or the backup
   chip was overwritten, use section 4 to put `vendor-main-1.rom` (or a
   known-good coreboot image) back on the main chip.
3. **Do not chase DDR or payload bugs with the programmer.** A hang
   after the SquirrelBIOS banner is a firmware problem, not a bad flash;
   keep the serial log and recover to vendor only if you need the
   machine back.

## 7. What “done” looks like on the desk

Owner checklist (`needs: squirrel`); not a CI gate:

- [ ] Two matching reads of vendor main and vendor backup stored off-board
- [ ] Main chip written and verified; backup chip untouched
- [ ] Serial console shows `SquirrelBIOS` and `mem: … (from coreboot tables)`
- [ ] A cold boot after DualBIOS recovery (or an external rewrite) can
      put the vendor image back from the saved dumps

## Sources

Public documentation summarised in our own words (no vendor BIOS or
coreboot source was copied into this tree):

- coreboot mainboard documentation for the GA-G41M-ES2L
  (https://doc.coreboot.org/)
- Libreboot / Canoeboot hardware and install notes for the board
- Gigabyte GA-G41M-ES2L user manual (SPI layout, DualBIOS, COM1 header)
- flashrom programmer documentation (`internal`, `ch341a_spi`, `linux_spi`)
- Intel G41 / 4 Series MCH and ICH7 datasheets (flash and LPC decode)
- SST25VF080B datasheet (JEDEC ID, 1 MB capacity, SPI commands)
