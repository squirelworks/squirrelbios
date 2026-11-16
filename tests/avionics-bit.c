/* Host stand-in for common/bit (ISM BIT line, #195). Prints markers
 * that tests/smoke.sh matches for case avionics-bit. */
#include <stdio.h>
#include <string.h>

#include "../common/bit/sq_bit.h"

static int expect_has(const char *line, const char *needle)
{
	return strstr(line, needle) != NULL;
}

int main(void)
{
	sq_bit_state_t st;
	char line[256];
	int n;
	int ok = 1;

	sq_bit_init(&st, "ISM-FJ-BAY-IO", "0.1.0");
	if (!sq_bit_set_act_centi(&st, 0, 50) || st.act_centi[0] != 50)
		ok = 0;
	if (!sq_bit_set_inc_centi(&st, 1, -25) || st.inc_centi[1] != -25)
		ok = 0;

	n = sq_bit_format_line(&st, line, (int)sizeof(line));
	if (n <= 0)
		ok = 0;
	if (!expect_has(line, "BIT board=ISM-FJ-BAY-IO"))
		ok = 0;
	if (!expect_has(line, "ver=0.1.0"))
		ok = 0;
	if (!expect_has(line, "rail28=1") || !expect_has(line, "rail5=1") ||
	    !expect_has(line, "rail3v3=1"))
		ok = 0;
	if (!expect_has(line, "imu=1") || !expect_has(line, "radio=1") ||
	    !expect_has(line, "uplink=1"))
		ok = 0;
	if (!expect_has(line, "brownout=0") || !expect_has(line, "safe=0"))
		ok = 0;
	if (!expect_has(line, "act=0.50,0.00,0.00,0.00"))
		ok = 0;
	if (!expect_has(line, "inc=0.00,-0.25,0.00,0.00"))
		ok = 0;

	/* Uplink loss → safe, actuators neutral, cmds rejected. */
	st.probes.uplink_ok = 0;
	sq_bit_apply_safe(&st);
	if (!st.safe_state || st.act_centi[0] != 0)
		ok = 0;
	if (sq_bit_set_act_centi(&st, 0, 80))
		ok = 0;
	n = sq_bit_format_line(&st, line, (int)sizeof(line));
	if (n <= 0 || !expect_has(line, "uplink=0") || !expect_has(line, "safe=1"))
		ok = 0;

	/* Brownout alone also forces safe. */
	sq_bit_init(&st, "ISM-FJ-BAY-IO", "0.1.0");
	sq_bit_set_act_centi(&st, 2, 10);
	st.probes.brownout = 1;
	sq_bit_apply_safe(&st);
	if (!st.safe_state || st.act_centi[2] != 0)
		ok = 0;

	if (!ok) {
		fputs("avionics-bit: FAIL\n", stderr);
		return 1;
	}
	fputs("avionics-bit fields=ok safe=ok\n", stdout);
	return 0;
}
