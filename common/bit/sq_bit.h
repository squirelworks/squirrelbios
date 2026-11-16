/* SquirrelBIOS portable BIT/POST result record (ISM avionics, #195).
 * Field names match ISM fighter-jet/interfaces/firmware-bringup.yaml
 * uart_stdio_profile.bit_line_fields. Original code; no third-party
 * firmware. Freestanding-friendly (no libc required in the library). */
#ifndef SQUIRRELBIOS_SQ_BIT_H
#define SQUIRRELBIOS_SQ_BIT_H

#define SQ_BIT_ACT_COUNT 4
#define SQ_BIT_INC_COUNT 4

/* Probe / health flags: 0 = fail or inactive, 1 = ok / active as named. */
typedef struct sq_bit_probes {
	int rail_28v_ok;
	int rail_5v_ok;
	int rail_3v3_ok;
	int imu_ok;
	int radio_ok;
	int uplink_ok;
	int brownout; /* 1 = brownout active */
} sq_bit_probes_t;

typedef struct sq_bit_state {
	const char *board_id;
	const char *version;
	sq_bit_probes_t probes;
	/* Actuator / inceptor vectors, stored as hundredths of a unit
	 * in [-100, 100] so the formatter needs no floating point. */
	int act_centi[SQ_BIT_ACT_COUNT];
	int inc_centi[SQ_BIT_INC_COUNT];
	int safe_state; /* 1 = safe (neutral actuators; cmds rejected) */
	int integrity_ok; /* 1 = boot image check passed (H2); default 1 */
} sq_bit_state_t;

void sq_bit_init(sq_bit_state_t *s, const char *board_id, const char *version);

/* Recompute safe_state from brownout, uplink loss, or integrity failure.
 * When entering safe, zeroes act_centi. */
void sq_bit_apply_safe(sq_bit_state_t *s);

/* Set one actuator channel in [-1.0, 1.0] via hundredths. Returns 0 if
 * rejected (safe_state or bad channel), 1 on success. */
int sq_bit_set_act_centi(sq_bit_state_t *s, int ch, int centi);

int sq_bit_set_inc_centi(sq_bit_state_t *s, int idx, int centi);

/* Write one BIT line (with trailing newline). Returns bytes written
 * (excluding NUL), or 0 if buf is too small / NULL. */
int sq_bit_format_line(const sq_bit_state_t *s, char *buf, int buflen);

#endif
