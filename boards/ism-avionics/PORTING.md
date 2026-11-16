# Porting guide: new ISM avionics boards on SquirrelBIOS

Tracker: [#195](https://github.com/squirelworks/squirrelbios/issues/195) H5.  
Read first: [`GAP.md`](GAP.md), [`../ism-rc-fc/README.md`](../ism-rc-fc/README.md),
`common/bit/README.md`.

## Steps

1. **Board card** under `boards/<name>/README.md`: board id string, MCU
   class, UART for BIT, probe table (rails, IMU, radio, uplink, brownout),
   safe-state mapping, RTOS image location.
2. **Board id + version** passed to `sq_bit_init`. Keep ids stable; they
   appear on the BIT line and in `sq_handoff_t.board_id`.
3. **Probes:** fill `sq_bit_probes_t` from GPIO/ADC/bus whoami. Call
   `sq_bit_apply_safe` after changes (or rely on set_act which re-applies).
4. **BIT line:** `sq_bit_format_line` → UART. Fields must stay aligned with
   ISM `firmware-bringup.yaml` `bit_line_fields` when the board is in that
   family (bay / FCC). RC may stub `rail28`.
5. **Integrity:** compute or embed expected FNV-1a (or later signature) of
   the RTOS/app image; `sq_integrity_check` before any jump. Failure leaves
   safe-state and forbids hand-off.
6. **Hand-off:** `sq_handoff_prepare` then branch to `entry` with the
   agreed stack. Document the block in `common/handoff.md` (MCU section).
   No BIOS services after the jump. Laws stay in the RTOS.
7. **Tests:** add a host smoke (`host_check <name>`) that formats BIT,
   fails integrity, and refuses hand-off. Tick ROADMAP/TODO/HANDOFF in the
   same PR. Run `make docs-lint` and `make test`.

## Do not

- Vendor SeaBIOS, EDK2, U-Boot, or Pico SDK trees into this repo.
- Put flight-control laws in the BIOS tree.
- Jump to an image that failed integrity or while safe-state is set.
