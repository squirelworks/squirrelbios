# Gap list: SquirrelBIOS as ISM avionics bring-up / BIT / boot integrity

Tracker: [squirrelbios#195](https://github.com/squirelworks/squirrelbios/issues/195).  
Interface contract (external):
[ISM `fighter-jet/interfaces/firmware-bringup.yaml`](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/interfaces/firmware-bringup.yaml)
(provisional stub, schema 1.0.0, issue #84). Platform intent:
[ISM `fighter-jet/avionics/computing-platform.md`](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/avionics/computing-platform.md).

SquirrelBIOS today is **PC / UEFI-class firmware for QEMU and a few desk
boards**. ISM wants it reused as the **early bring-up, built-in test
(BIT/POST), and boot-integrity layer** on avionics MCUs, then a clean
hand-off to a **separate small RTOS** that owns flight-control laws.
This file lists what those boards need that we do not yet provide.

## Target boards (what they need)

| Board / LRU | MCU class (ISM today) | Role | BIOS must provide |
|---|---|---|---|
| Bay I/O concentrator (`ISM-FJ-BAY-IO`) | RP2040-class (Pico SDK deferred) | Study / HIL I/O, not sole FCC path | Board-id, rail probes, IMU/radio/uplink/brownout BIT, safe-state, UART BIT line, hand-off to bay app/RTOS |
| FCC lanes (×4) | TBD (avionics MCU; STM32-class likely) | Flight-critical laws on RTOS | Same BIT + integrity + hand-off; no laws in BIOS |
| Mission computer | Separate LRU | Mission apps | Out of BIOS scope beyond optional future MC BIT topic |
| **Eric’s ISM RC flight controller** | Consumer/RC MCU (trial target) | First SquirrelBIOS trial | Same framework; RC-specific board-id and probe table; lessons feed bay/FCC |

Flight laws stay on the RTOS. BIOS does early init, BIT, integrity, then
jumps. See ISM computing-platform §1 and §3.

## Machine-readable BIT record (ISM contract)

`firmware-bringup.yaml` `uart_stdio_profile.bit_line_fields` (host
stdio substitutes the bus until ISM-Bus-A lands):

| Field | Meaning |
|---|---|
| `board` | Board id string (bay: `ISM-FJ-BAY-IO`) |
| `ver` | Firmware version string |
| `rail28` / `rail5` / `rail3v3` | Power-rail OK flags |
| `imu` / `radio` / `uplink` | Probe OK flags |
| `brownout` | Brownout active |
| `safe` | Safe-state engaged |
| `act_vector` | Actuator cmds (−1…+1), J1–J4 / trunks A1–A4 |
| `inc_vector` | Inceptor stubs (stick pitch/roll, throttle, pedal) |

Safe-state triggers in the yaml: `brownout`, `uplink_loss` → neutral all
actuator cmds; further cmds rejected while safe. BIT topic alias:
`bay_io.bit` @ ~10 Hz on ISM-Bus-A.

Reference line shape (ISM host stub today):

```text
BIT board=ISM-FJ-BAY-IO ver=0.1.0 rail28=1 rail5=1 rail3v3=1 imu=1 radio=1 uplink=1 brownout=0 safe=0 act=0.00,0.00,0.00,0.00 inc=0.00,0.00,0.00,0.00
```

## What SquirrelBIOS already has (reuse)

| Capability | Where | Useful for avionics? |
|---|---|---|
| Early init → load image → jump; never jump to a partial image | `common/handoff.md`, all platforms | Yes — same failure rule |
| Failure = console message + halt | every platform | Partial — avionics also need **safe-state** (neutral outputs), not only halt |
| Serial banner / console | COM1, PL011, NS16550A, … | Yes — BIT line rides stdio/UART |
| Secure Boot / Authenticode (UEFI) | `x86_64/secure/` | Pattern for image verify; MCU path needs a **small hash/signature** check, not full UEFI |
| TPM measured boot (UEFI) | `x86_64` TPM | Overkill for first MCU trial; optional later |
| POST-shaped x86 init | `x86/core/`, drivers | PC-only; not portable to RP2040/STM32 |
| Real-board research tree | `boards/` | Pattern for `boards/ism-*` notes |

## Gaps (ordered for #195)

### 1. Portable BIT/POST framework + result record — **missing**

- No shared, architecture-neutral BIT state or formatter whose fields
  match `bit_line_fields` above.
- No board-id / version table driven by a board config.
- No probe hooks for rails, IMU, radio, uplink, brownout (today’s POST
  is PC/UEFI device enumeration).
- No host-buildable stand-in that proves the record without QEMU MCU
  models (ISM already uses a host `make test` for bay firmware).

### 2. Boot-integrity check + safe-state on failure — **missing**

- UEFI Secure Boot verifies PE images; there is no MCU-sized “hash or
  signature of the RTOS/app image, then refuse hand-off” path.
- On verify failure we halt; we do not enter an avionics **safe-state**
  (neutral actuators / reject cmds) as in `firmware-bringup.yaml`
  `safe_state`.
- No recorded integrity result in the BIT line.

### 3. Clean RTOS / application hand-off — **partial**

- PC/UEFI/SBI hand-offs are documented and tested; there is no small
  **MCU hand-off block**: entry PC, stack, BIT record pointer, board-id,
  integrity status, “BIOS done / no further services” rule for an RTOS
  that is not Linux/OS/2.
- Laws must not live in BIOS; the contract must say what registers /
  memory the RTOS may assume.

### 4. First target: Eric’s ISM RC flight controller — **missing**

- No `boards/` note, board-id, probe table, or build config for the RC
  controller.
- MCU class and pin map are not pinned in this repo yet (Eric’s lane;
  ISM publishes geometry/CG targets only). Gap closure needs a board
  card that lists assumed MCU class, UART for BIT, rail/IMU/radio
  probes, and a host stand-in until hardware arrives.

### 5. Porting guide for new avionics boards — **missing**

- `boards/README.md` covers Pi / VisionFive / HiFive / GA-G41M only.
- Need a how-to: add board-id, fill probe table, wire BIT line, hook
  integrity, hand off to RTOS, add `make test` / host check.

### 6. MCU platform build (follow-on, not a #195 checkbox by itself)

- No `PLATFORM=` for Cortex-M / RP2040. First PRs should land **portable
  C + host tests + board docs**; a QEMU/`pico` image is a later step
  once the RC board card pins the MCU.

## Non-goals (keep out of BIOS)

- Flight-control laws, envelope limiters, vote monitors (RTOS / FCC).
- Soft desktop / Flight Lab UI (SquirrelSoft / OS/2 ground lane).
- Vendoring SeaBIOS, EDK2, U-Boot, Pico SDK trees into this repo.
- Copying ISM bay application logic wholesale; share the **BIT line
  contract**, not their HIL command shell.

## Proposed landings (one PR each, matching #195)

1. **This gap list** (`boards/ism-avionics/GAP.md` + ROADMAP/TODO/HANDOFF).
2. **`common/bit/`** portable BIT state + formatter + host unit test
   matching `bit_line_fields`.
3. **Integrity + safe-state** APIs and host tests (fail → safe + no jump).
4. **RTOS hand-off header / contract** in `common/handoff.md` (MCU
   section) + stand-in.
5. **`boards/ism-rc-fc/`** first target card + board-id wiring.
6. **Porting guide** `boards/ism-avionics/PORTING.md`.

## References

- Issue #195 (this work).
- ISM #84 bay firmware bring-up; `fighter-jet/firmware/` host stub.
- squirrel-os2 #225 (OS/2 as ground/lab platform).
