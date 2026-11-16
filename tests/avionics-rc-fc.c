/* Host stand-in: Eric's ISM RC flight controller board id (#195 H4). */
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
	char line[288];
	unsigned char app[] = { 0x52, 0x43, 0x46, 0x43, 0x01 }; /* "RCFC" */
	unsigned long long expect;
	int ok = 1;

	sq_bit_init(&st, "ISM-RC-FC", "0.1.0");
	/* RC often has no 28 V aircraft rail — stub ok. */
	st.probes.rail_28v_ok = 1;
	expect = sq_integrity_hash(app, sizeof(app));
	img.data = app;
	img.len = sizeof(app);
	if (!sq_integrity_check(&st, &img, expect))
		ok = 0;
	if (sq_bit_format_line(&st, line, (int)sizeof(line)) <= 0)
		ok = 0;
	if (!strstr(line, "BIT board=ISM-RC-FC") || !strstr(line, "ver=0.1.0"))
		ok = 0;
	if (!strstr(line, "safe=0"))
		ok = 0;
	if (!sq_handoff_prepare(&ho, &st, 0x10000100u, 0x20004000u))
		ok = 0;
	if (strcmp(ho.board_id, "ISM-RC-FC") != 0)
		ok = 0;

	if (!ok) {
		fputs("avionics-rc-fc: FAIL\n", stderr);
		return 1;
	}
	fputs("avionics-rc-fc board=ok handoff=ok\n", stdout);
	return 0;
}
