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
This file lists what those boards need and what has landed under #195.

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
| `watchdog` / `incloss` | Watchdog reset hold / inceptor loss (H7) |
| `safe` | Safe-state engaged |
| `act_vector` | Actuator cmds (−1…+1), J1–J4 / trunks A1–A4 |
| `inc_vector` | Inceptor stubs (stick pitch/roll, throttle, pedal) |

Safe-state triggers in the yaml: `brownout`, `uplink_loss`,
`watchdog_reset`, `inceptor_loss` → neutral all actuator cmds; further
cmds rejected while safe. Portable probes carry all four; UART keys
`watchdog=` / `incloss=` expose the extras on the host BIT line (H7).
BIT topic alias: `bay_io.bit` @ ~10 Hz on ISM-Bus-A.

Reference line shape (SquirrelBIOS host formatter):

```text
BIT board=ISM-FJ-BAY-IO ver=0.2.0 rail28=1 rail5=1 rail3v3=1 imu=1 radio=1 uplink=1 brownout=0 watchdog=0 incloss=0 safe=0 act=0.00,0.00,0.00,0.00 inc=0.00,0.00,0.00,0.00
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
| Portable BIT / integrity / SQHO | `common/bit/` | Host-tested MCU bring-up path (#195 H1–H3) |

## Gaps (ordered for #195)

### 1. Portable BIT/POST framework + result record — **landed (H1)**

- `common/bit/sq_bit.*` formats the ISM field set; smoke `avionics-bit`.

### 2. Boot-integrity check + safe-state on failure — **landed (H2)**

- `sq_integrity_*` FNV-1a check; fail → safe + no hand-off; smoke
  `avionics-integrity`.

### 3. Clean RTOS / application hand-off — **landed (H3)**

- `sq_handoff_t` (`SQHO`) + MCU section in `common/handoff.md`; smoke
  `avionics-handoff`.

### 4. First target: Eric’s ISM RC flight controller — **landed (H4)**

- `boards/ism-rc-fc/`; smoke `avionics-rc-fc`.

### 5. Porting guide for new avionics boards — **landed (H5)**

- `boards/ism-avionics/PORTING.md`.

### 6. Second target: bay I/O concentrator — **landed (H6)**

- `boards/ism-fj-bay-io/` board card (`ISM-FJ-BAY-IO`); smoke
  `avionics-bay-io` (rails, uplink loss → safe → recovery, integrity
  hand-off).

### 7. MCU platform build (follow-on, not a #195 checkbox by itself)

- No `PLATFORM=` for Cortex-M / RP2040. First PRs landed **portable
  C + host tests + board docs**; a QEMU/`pico` image is a later step
  once the RC/bay board cards pin flash layout.

### 8. Extra ISM safe-state triggers — **landed (H7)**

- `sq_bit_probes_t.watchdog_reset` / `inceptor_loss` force safe-state;
  BIT line emits `watchdog=` / `incloss=`; smoke `avionics-bit` covers
  engage + recovery for each.

### 9. FCC lane board cards (open)

- No `boards/ism-fj-fcc-*` cards yet. Same BIT + integrity + hand-off
  contract; laws stay on the RTOS. Host-testable when a board id and
  probe table are pinned.

## Non-goals (keep out of BIOS)

- Flight-control laws, envelope limiters, vote monitors (RTOS / FCC).
- Soft desktop / Flight Lab UI (SquirrelSoft / OS/2 ground lane).
- Vendoring SeaBIOS, EDK2, U-Boot, Pico SDK trees into this repo.
- Copying ISM bay application logic wholesale; share the **BIT line
  contract**, not their HIL command shell.

## Proposed landings (one PR each, matching #195)

1. ~~**This gap list**~~ (`boards/ism-avionics/GAP.md` + ROADMAP/TODO/HANDOFF) — H0.
2. ~~**`common/bit/`** portable BIT state + formatter + host unit test~~ — H1.
3. ~~**Integrity + safe-state** APIs and host tests~~ — H2.
4. ~~**RTOS hand-off header / contract**~~ — H3.
5. ~~**`boards/ism-rc-fc/`** first target card~~ — H4.
6. ~~**Porting guide** `boards/ism-avionics/PORTING.md`~~ — H5.
7. ~~**`boards/ism-fj-bay-io/`** bay I/O second target~~ — H6.
8. ~~**Extra safe-state probe flags** (watchdog / inceptor_loss)~~ — H7.
9. **FCC lane board card(s)** when ids/probes are pinned (host smoke).
10. **MCU `PLATFORM=`** when silicon / flash layout is pinned (desk /
    QEMU later; not a merge blocker alone).

## References

- Issue #195 (this work).
- ISM #84 bay firmware bring-up; `fighter-jet/firmware/` host stub.
- squirrel-os2 #225 (OS/2 as ground/lab platform).
