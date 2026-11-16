/* MCU / RTOS hand-off block for avionics (#195 H3). */
#ifndef SQUIRRELBIOS_SQ_HANDOFF_H
#define SQUIRRELBIOS_SQ_HANDOFF_H

#include "sq_bit.h"

#define SQ_HANDOFF_MAGIC 0x5351484FU /* 'SQHO' */
#define SQ_HANDOFF_VERSION 1u

typedef struct sq_handoff {
	unsigned int magic;
	unsigned int version;
	unsigned int entry;
	unsigned int stack;
	unsigned int bit_addr;
	unsigned int flags; /* bit0 = integrity_ok */
	char board_id[32];
} sq_handoff_t;

int sq_handoff_prepare(sq_handoff_t *out, const sq_bit_state_t *bit,
		       unsigned int entry, unsigned int stack);

#endif
