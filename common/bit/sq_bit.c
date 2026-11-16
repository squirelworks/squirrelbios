/* SquirrelBIOS portable BIT/POST result record (#195). */
#include "sq_bit.h"

static int clamp_centi(int v)
{
	if (v < -100)
		return -100;
	if (v > 100)
		return 100;
	return v;
}

static void zero_acts(sq_bit_state_t *s)
{
	int i;

	for (i = 0; i < SQ_BIT_ACT_COUNT; i++)
		s->act_centi[i] = 0;
}

void sq_bit_init(sq_bit_state_t *s, const char *board_id, const char *version)
{
	int i;

	if (!s)
		return;
	s->board_id = board_id ? board_id : "UNKNOWN";
	s->version = version ? version : "0.0.0";
	s->probes.rail_28v_ok = 1;
	s->probes.rail_5v_ok = 1;
	s->probes.rail_3v3_ok = 1;
	s->probes.imu_ok = 1;
	s->probes.radio_ok = 1;
	s->probes.uplink_ok = 1;
	s->probes.brownout = 0;
	s->probes.watchdog_reset = 0;
	s->probes.inceptor_loss = 0;
	for (i = 0; i < SQ_BIT_ACT_COUNT; i++)
		s->act_centi[i] = 0;
	for (i = 0; i < SQ_BIT_INC_COUNT; i++)
		s->inc_centi[i] = 0;
	s->safe_state = 0;
	s->integrity_ok = 1;
	sq_bit_apply_safe(s);
}

void sq_bit_apply_safe(sq_bit_state_t *s)
{
	int need_safe;

	if (!s)
		return;
	need_safe = s->probes.brownout || !s->probes.uplink_ok ||
		    s->probes.watchdog_reset || s->probes.inceptor_loss ||
		    !s->integrity_ok;
	if (need_safe) {
		s->safe_state = 1;
		zero_acts(s);
	} else {
		s->safe_state = 0;
	}
}

int sq_bit_set_act_centi(sq_bit_state_t *s, int ch, int centi)
{
	if (!s || ch < 0 || ch >= SQ_BIT_ACT_COUNT)
		return 0;
	sq_bit_apply_safe(s);
	if (s->safe_state)
		return 0;
	s->act_centi[ch] = clamp_centi(centi);
	return 1;
}

int sq_bit_set_inc_centi(sq_bit_state_t *s, int idx, int centi)
{
	if (!s || idx < 0 || idx >= SQ_BIT_INC_COUNT)
		return 0;
	s->inc_centi[idx] = clamp_centi(centi);
	return 1;
}

/* Tiny freestanding helpers (no libc). */
static int append_char(char *buf, int buflen, int pos, char c)
{
	if (pos < 0 || pos + 1 >= buflen)
		return -1;
	buf[pos] = c;
	return pos + 1;
}

static int append_str(char *buf, int buflen, int pos, const char *str)
{
	if (!str)
		str = "";
	while (*str) {
		pos = append_char(buf, buflen, pos, *str++);
		if (pos < 0)
			return -1;
	}
	return pos;
}

static int append_int(char *buf, int buflen, int pos, int v)
{
	char tmp[16];
	int n = 0;
	int neg = 0;
	unsigned u;

	if (v < 0) {
		neg = 1;
		u = (unsigned)(-(v + 1)) + 1u;
	} else {
		u = (unsigned)v;
	}
	if (u == 0) {
		tmp[n++] = '0';
	} else {
		while (u > 0 && n < (int)sizeof(tmp)) {
			tmp[n++] = (char)('0' + (u % 10u));
			u /= 10u;
		}
	}
	if (neg) {
		pos = append_char(buf, buflen, pos, '-');
		if (pos < 0)
			return -1;
	}
	while (n > 0) {
		pos = append_char(buf, buflen, pos, tmp[--n]);
		if (pos < 0)
			return -1;
	}
	return pos;
}

/* Format hundredths as [-]N.NN */
static int append_centi(char *buf, int buflen, int pos, int centi)
{
	int neg = 0;
	int whole;
	int frac;

	centi = clamp_centi(centi);
	if (centi < 0) {
		neg = 1;
		centi = -centi;
	}
	whole = centi / 100;
	frac = centi % 100;
	if (neg) {
		pos = append_char(buf, buflen, pos, '-');
		if (pos < 0)
			return -1;
	}
	pos = append_int(buf, buflen, pos, whole);
	if (pos < 0)
		return -1;
	pos = append_char(buf, buflen, pos, '.');
	if (pos < 0)
		return -1;
	pos = append_char(buf, buflen, pos, (char)('0' + (frac / 10)));
	if (pos < 0)
		return -1;
	return append_char(buf, buflen, pos, (char)('0' + (frac % 10)));
}

int sq_bit_format_line(const sq_bit_state_t *s, char *buf, int buflen)
{
	int pos = 0;
	int i;

	if (!s || !buf || buflen < 8)
		return 0;

	pos = append_str(buf, buflen, pos, "BIT board=");
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, s->board_id);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " ver=");
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, s->version);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " rail28=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.rail_28v_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " rail5=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.rail_5v_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " rail3v3=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.rail_3v3_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " imu=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.imu_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " radio=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.radio_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " uplink=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.uplink_ok ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " brownout=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.brownout ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " watchdog=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.watchdog_reset ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " incloss=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->probes.inceptor_loss ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " safe=");
	if (pos < 0)
		return 0;
	pos = append_int(buf, buflen, pos, s->safe_state ? 1 : 0);
	if (pos < 0)
		return 0;
	pos = append_str(buf, buflen, pos, " act=");
	if (pos < 0)
		return 0;
	for (i = 0; i < SQ_BIT_ACT_COUNT; i++) {
		if (i) {
			pos = append_char(buf, buflen, pos, ',');
			if (pos < 0)
				return 0;
		}
		pos = append_centi(buf, buflen, pos, s->act_centi[i]);
		if (pos < 0)
			return 0;
	}
	pos = append_str(buf, buflen, pos, " inc=");
	if (pos < 0)
		return 0;
	for (i = 0; i < SQ_BIT_INC_COUNT; i++) {
		if (i) {
			pos = append_char(buf, buflen, pos, ',');
			if (pos < 0)
				return 0;
		}
		pos = append_centi(buf, buflen, pos, s->inc_centi[i]);
		if (pos < 0)
			return 0;
	}
	pos = append_char(buf, buflen, pos, '\n');
	if (pos < 0)
		return 0;
	if (pos >= buflen)
		return 0;
	buf[pos] = '\0';
	return pos;
}
