/* Boot-image integrity check for avionics hand-off (#195 H2).
 * Original code. FNV-1a 64-bit digest vs expected value. */
#ifndef SQUIRRELBIOS_SQ_INTEGRITY_H
#define SQUIRRELBIOS_SQ_INTEGRITY_H

#include "sq_bit.h"

typedef struct sq_image_view {
	const unsigned char *data;
	unsigned long len;
} sq_image_view_t;

unsigned long long sq_integrity_hash(const unsigned char *data,
				     unsigned long len);

/* On mismatch: clear integrity_ok, safe-state, return 0. Else 1. */
int sq_integrity_check(sq_bit_state_t *bit, const sq_image_view_t *img,
		       unsigned long long expected_hash);

int sq_integrity_may_handoff(const sq_bit_state_t *bit);

#endif
