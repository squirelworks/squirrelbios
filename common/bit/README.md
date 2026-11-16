# Portable BIT / POST result record

Architecture-neutral built-in test state and a machine-readable BIT line
for ISM avionics bring-up ([#195](https://github.com/squirelworks/squirrelbios/issues/195)).

Field names match
[ISM `firmware-bringup.yaml`](https://github.com/squirelworks/international-squirrel-machines/blob/main/fighter-jet/interfaces/firmware-bringup.yaml)
`uart_stdio_profile` / `bit-frame.yaml` host substitute: `board`, `ver`,
`rail28`, `rail5`, `rail3v3`, `imu`, `radio`, `uplink`, `brownout`,
`watchdog`, `incloss`, `safe`, `act_vector`, `inc_vector`.

| File | Role |
|---|---|
| `sq_bit.h` / `sq_bit.c` | State, safe-state from brownout / uplink / watchdog / inceptor / integrity, formatter |
| `tests/avionics-bit.c` | Host stand-in; smoke case `avionics-bit` |

Safe-state (neutral actuators, reject cmds) engages when brownout is
active, uplink is lost, `watchdog_reset` or `inceptor_loss` is set, or
`integrity_ok` is clear. UART keys `watchdog=` / `incloss=` mirror ISM
`safe_state.triggers` `watchdog_reset` / `inceptor_loss` (#195 H7).

| `sq_integrity.h` / `.c` | FNV-1a image hash; safe-state + no hand-off on mismatch |
| `sq_handoff.h` / `.c` | `SQHO` hand-off block for an RTOS/app entry |

Host smokes: `avionics-bit`, `avionics-integrity`, `avionics-handoff`,
`avionics-rc-fc`, `avionics-bay-io`.
