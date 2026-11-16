/* Host stand-in: RTOS hand-off block (#195 H3). */
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
	unsigned char img_bytes[] = { 'R', 'T', 'O', 'S' };
	unsigned long long expect;
	int ok = 1;

	sq_bit_init(&st, "ISM-RC-FC", "0.1.0");
	expect = sq_integrity_hash(img_bytes, sizeof(img_bytes));
	img.data = img_bytes;
	img.len = sizeof(img_bytes);
	if (!sq_integrity_check(&st, &img, expect))
		ok = 0;
	memset(&ho, 0, sizeof(ho));
	if (!sq_handoff_prepare(&ho, &st, 0x10008000u, 0x20000000u))
		ok = 0;
	if (ho.magic != SQ_HANDOFF_MAGIC || ho.version != SQ_HANDOFF_VERSION)
		ok = 0;
	if (ho.entry != 0x10008000u || ho.stack != 0x20000000u)
		ok = 0;
	if ((ho.flags & 1u) == 0)
		ok = 0;
	if (strcmp(ho.board_id, "ISM-RC-FC") != 0)
		ok = 0;

	/* Bad integrity → prepare must refuse. */
	st.integrity_ok = 0;
	sq_bit_apply_safe(&st);
	if (sq_handoff_prepare(&ho, &st, 0x10008000u, 0))
		ok = 0;

	if (!ok) {
		fputs("avionics-handoff: FAIL\n", stderr);
		return 1;
	}
	fputs("avionics-handoff prepare=ok refuse=ok\n", stdout);
	return 0;
}
