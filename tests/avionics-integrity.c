/* Host stand-in: boot integrity + safe-state (#195 H2). */
#include <stdio.h>
#include <string.h>

#include "../common/bit/sq_bit.h"
#include "../common/bit/sq_integrity.h"

int main(void)
{
	sq_bit_state_t st;
	sq_image_view_t img;
	unsigned char good[] = { 0x01, 0x02, 0x03, 0x53, 0x51 };
	unsigned char bad[] = { 0x01, 0x02, 0x03, 0x00, 0x00 };
	unsigned long long expect;
	int ok = 1;

	sq_bit_init(&st, "ISM-RC-FC", "0.1.0");
	sq_bit_set_act_centi(&st, 0, 40);
	expect = sq_integrity_hash(good, sizeof(good));
	img.data = good;
	img.len = sizeof(good);
	if (!sq_integrity_check(&st, &img, expect))
		ok = 0;
	if (!sq_integrity_may_handoff(&st) || st.safe_state)
		ok = 0;

	img.data = bad;
	img.len = sizeof(bad);
	if (sq_integrity_check(&st, &img, expect))
		ok = 0;
	if (st.integrity_ok || !st.safe_state || st.act_centi[0] != 0)
		ok = 0;
	if (sq_integrity_may_handoff(&st))
		ok = 0;
	if (sq_bit_set_act_centi(&st, 0, 10))
		ok = 0;

	if (!ok) {
		fputs("avionics-integrity: FAIL\n", stderr);
		return 1;
	}
	fputs("avionics-integrity hash=ok safe=ok no-handoff=ok\n", stdout);
	return 0;
}
