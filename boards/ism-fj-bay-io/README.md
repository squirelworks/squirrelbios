# ISM fighter-jet bay I/O concentrator (second SquirrelBIOS target)

Tracker: [squirrelbios#195](https://github.com/squirelworks/squirrelbios/issues/195) H6.  
Contract: [ISM `firmware-bringup.yaml`](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/interfaces/firmware-bringup.yaml)
(board id `ISM-FJ-BAY-IO`). Intent:
[ISM computing-platform.md](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/avionics/computing-platform.md).

Study / HIL I/O concentrator for the fighter-jet bay — **not** the sole
FCC path. SquirrelBIOS provides bring-up, BIT, boot integrity, and a
clean hand-off to a bay app / small RTOS. Flight laws stay on FCC lanes.

## Board identity

| Field | Value |
|---|---|
| Board id | `ISM-FJ-BAY-IO` |
| Firmware ver (bring-up) | `0.2.0` (matches ISM yaml `board.firmware_version`) |
| MCU class | RP2040-class (Pico SDK target deferred; host stand-in first) |
| Console | UART stdio BIT line until ISM-Bus-A `bay_io.bit` @ ~10 Hz |
| Role | Bay I/O concentrator / soft-HIL; not FCC laws |

Pin maps and KiCad live in the ISM `fighter-jet/` tree. This card pins
the **BIOS-facing** contract so host tests and a later MCU `PLATFORM=`
share one board id.

## Probe table (BIT)

| Probe | Source (bring-up) | Notes |
|---|---|---|
| `rail28` | Aircraft 28 V sense | Required on bay (unlike many RC packs) |
| `rail5` / `rail3v3` | Regulator OK sense | Required for safe logic |
| `imu` | IMU whoami / bus ACK | Fail → BIT shows imu=0 |
| `radio` | Radio / link probe | Fail → radio=0 |
| `uplink` | Valid uplink / host cmds recently | Loss → safe-state |
| `brownout` | BOD / 28 V under-voltage | Active → safe-state |
| `watchdog` | MCU WDT timeout / post-reset hold | Active → safe-state |
| `incloss` | Inceptor validity lost | Active → safe-state |

Actuator vector: J1–J4 (trunks A1–A4), hundredths in `[-1.0, 1.0]`.  
Inceptor stubs: stick pitch/roll, throttle, pedal yaw as `inc`.

## Safe-state

On brownout, uplink loss, watchdog reset, inceptor loss, or integrity
failure: neutral all actuator cmds, reject new cmds, **do not jump** to
the bay app/RTOS. Matches ISM `firmware-bringup.yaml` `safe_state`
triggers (`brownout`, `uplink_loss`, `watchdog_reset`, `inceptor_loss`).

## Host check

Smoke case `avionics-bay-io` builds the BIT line with board id
`ISM-FJ-BAY-IO` / `ver=0.2.0`, exercises uplink loss → safe → recovery,
and runs integrity + hand-off prepare against a synthetic bay image.

## Out of scope here

- Bay application / HIL command shell (ISM `fighter-jet/firmware/`).
- FCC lane laws or vote monitors.
- QEMU RP2040 machine (later `PLATFORM=` once the board tree pins flash
  layout).
