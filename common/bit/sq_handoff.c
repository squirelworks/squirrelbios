/* MCU / RTOS hand-off block (#195 H3). */
#include "sq_handoff.h"

#include "sq_integrity.h"

static void copy_id(char *dst, const char *src, unsigned n)
{
	unsigned i = 0;

	if (!dst || n == 0)
		return;
	while (i + 1 < n && src && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

int sq_handoff_prepare(sq_handoff_t *out, const sq_bit_state_t *bit,
		       unsigned int entry, unsigned int stack)
{
	if (!out || !bit)
		return 0;
	if (!sq_integrity_may_handoff(bit))
		return 0;
	if (entry == 0)
		return 0;

	out->magic = SQ_HANDOFF_MAGIC;
	out->version = SQ_HANDOFF_VERSION;
	out->entry = entry;
	out->stack = stack;
	out->bit_addr = 0;
	out->flags = bit->integrity_ok ? 1u : 0u;
	copy_id(out->board_id, bit->board_id, (unsigned)sizeof(out->board_id));
	return 1;
}
