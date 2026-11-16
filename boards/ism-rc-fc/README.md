# Eric’s ISM RC flight controller (first SquirrelBIOS trial)

Tracker: [squirrelbios#195](https://github.com/squirelworks/squirrelbios/issues/195) H4.  
Intent: [ISM computing-platform.md §3](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/avionics/computing-platform.md).

First place to trial the portable BIT / integrity / RTOS hand-off stack
(`common/bit/`) before bay I/O and FCC LRUs. **Flight laws stay on a
separate small RTOS**; SquirrelBIOS does bring-up, BIT, integrity, then
hands off.

## Board identity

| Field | Value |
|---|---|
| Board id | `ISM-RC-FC` |
| Firmware ver (bring-up) | `0.1.0` |
| MCU class (assumed until Eric pins silicon) | RP2040-class or STM32-class Cortex-M |
| Console | UART stdio for the BIT line (same field set as ISM bay) |
| App | Small RTOS + RC control laws (out of this repo) |

Pin maps and exact part numbers are Eric’s lane; this card only pins the
**BIOS-facing** contract so host tests and later MCU ports share one id.

## Probe table (BIT)

| Probe | Source (bring-up) | Notes |
|---|---|---|
| `rail28` | N/A on many RC packs → report 1 when unused, or map to pack sense | Bay uses 28 V aircraft; RC may stub |
| `rail5` / `rail3v3` | Regulator OK sense or stub | Required for safe logic |
| `imu` | IMU whoami / bus ACK | Fail → BIT shows imu=0 |
| `radio` | RC link / ELRS / CRSF probe | Fail → radio=0 |
| `uplink` | Valid RC frames recently | Loss → safe-state |
| `brownout` | BOD / pack under-voltage | Active → safe-state |
| `watchdog` | MCU WDT timeout / post-reset hold | Active → safe-state |
| `incloss` | Stick / RC inceptor validity lost | Active → safe-state |

Actuator vector: four channels (e.g. throttle / roll / pitch / yaw) as
`act` hundredths. Inceptors optional on RC (may mirror sticks as `inc`).

## Safe-state

On brownout, uplink loss, watchdog reset, inceptor loss, or integrity
failure: neutral actuators, reject new cmds, **do not jump** to the
RTOS. Matches ISM `firmware-bringup.yaml` `safe_state` intent.

## Host check

Smoke case `avionics-rc-fc` builds the BIT line with board id `ISM-RC-FC`
and runs integrity + hand-off prepare against a synthetic image.

## Out of scope here

- RC product application code or shop machine designs.
- Pulling RC sources into ISM `fighter-jet/`.
- QEMU RP2040/STM32 machine (later `PLATFORM=` once silicon is pinned).
