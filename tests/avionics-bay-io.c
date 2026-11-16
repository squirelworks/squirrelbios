/* Host stand-in: ISM fighter-jet bay I/O concentrator (#195 H6). */
#include <stdio.h>
#include <string.h>

#include "../common/bit/sq_bit.h"
#include "../common/bit/sq_handoff.h"
#include "../common/bit/sq_integrity.h"

int main(void)
{
	sq_bit_state_t st;
	sq_handoff_t ho;
	sq_image_view_t img;
	char line[256];
	/* Synthetic bay app image tag "BAYI" + rev. */
	unsigned char app[] = { 0x42, 0x41, 0x59, 0x49, 0x02 };
	unsigned long long expect;
	int ok = 1;

	sq_bit_init(&st, "ISM-FJ-BAY-IO", "0.2.0");
	/* Aircraft bay: all three rails must be present. */
	st.probes.rail_28v_ok = 1;
	st.probes.rail_5v_ok = 1;
	st.probes.rail_3v3_ok = 1;
	st.probes.imu_ok = 1;
	st.probes.radio_ok = 1;
	st.probes.uplink_ok = 1;
	st.probes.brownout = 0;
	sq_bit_apply_safe(&st);

	/* J1–J4 / inceptor stubs while healthy. */
	if (!sq_bit_set_act_centi(&st, 0, 25) || !sq_bit_set_act_centi(&st, 3, -10))
		ok = 0;
	if (!sq_bit_set_inc_centi(&st, 0, 5) || !sq_bit_set_inc_centi(&st, 2, 100))
		ok = 0;

	expect = sq_integrity_hash(app, sizeof(app));
	img.data = app;
	img.len = sizeof(app);
	if (!sq_integrity_check(&st, &img, expect))
		ok = 0;

	if (sq_bit_format_line(&st, line, (int)sizeof(line)) <= 0)
		ok = 0;
	if (!strstr(line, "BIT board=ISM-FJ-BAY-IO") ||
	    !strstr(line, "ver=0.2.0"))
		ok = 0;
	if (!strstr(line, "rail28=1") || !strstr(line, "rail5=1") ||
	    !strstr(line, "rail3v3=1"))
		ok = 0;
	if (!strstr(line, "safe=0") ||
	    !strstr(line, "act=0.25,0.00,0.00,-0.10") ||
	    !strstr(line, "inc=0.05,0.00,1.00,0.00"))
		ok = 0;

	if (!sq_handoff_prepare(&ho, &st, 0x10000200u, 0x20008000u))
		ok = 0;
	if (strcmp(ho.board_id, "ISM-FJ-BAY-IO") != 0)
		ok = 0;

	/* Uplink loss → safe, actuators neutral, cmds rejected. */
	st.probes.uplink_ok = 0;
	sq_bit_apply_safe(&st);
	if (!st.safe_state || st.act_centi[0] != 0 || st.act_centi[3] != 0)
		ok = 0;
	if (sq_bit_set_act_centi(&st, 1, 40))
		ok = 0;
	if (sq_handoff_prepare(&ho, &st, 0x10000200u, 0x20008000u))
		ok = 0; /* must refuse while safe */
	if (sq_bit_format_line(&st, line, (int)sizeof(line)) <= 0 ||
	    !strstr(line, "uplink=0") || !strstr(line, "safe=1"))
		ok = 0;

	/* Restore uplink → leave safe; cmds accepted again. */
	st.probes.uplink_ok = 1;
	sq_bit_apply_safe(&st);
	if (st.safe_state)
		ok = 0;
	if (!sq_bit_set_act_centi(&st, 1, 40) || st.act_centi[1] != 40)
		ok = 0;
	if (!sq_handoff_prepare(&ho, &st, 0x10000200u, 0x20008000u))
		ok = 0;

	if (!ok) {
		fputs("avionics-bay-io: FAIL\n", stderr);
		return 1;
	}
	fputs("avionics-bay-io board=ok safe=ok handoff=ok\n", stdout);
	return 0;
}
