/* Boot-image integrity check (#195 H2). */
#include "sq_integrity.h"

unsigned long long sq_integrity_hash(const unsigned char *data,
				     unsigned long len)
{
	unsigned long long h = 14695981039346656037ULL;
	unsigned long i;

	if (!data && len)
		return h;
	for (i = 0; i < len; i++) {
		h ^= (unsigned long long)data[i];
		h *= 1099511628211ULL;
	}
	return h;
}

int sq_integrity_check(sq_bit_state_t *bit, const sq_image_view_t *img,
		       unsigned long long expected_hash)
{
	unsigned long long got;

	if (!bit || !img)
		return 0;
	got = sq_integrity_hash(img->data, img->len);
	if (got != expected_hash) {
		bit->integrity_ok = 0;
		sq_bit_apply_safe(bit);
		return 0;
	}
	bit->integrity_ok = 1;
	sq_bit_apply_safe(bit);
	return 1;
}

int sq_integrity_may_handoff(const sq_bit_state_t *bit)
{
	if (!bit)
		return 0;
	return bit->integrity_ok && !bit->safe_state;
}
