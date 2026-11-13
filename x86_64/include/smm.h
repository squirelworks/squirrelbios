/* SquirrelBIOS x86_64-uefi: System Management Mode on q35 and the EINJ
 * error injection it serves (roadmap F2). Original code. */
#ifndef SMM_H
#define SMM_H

#include "efi.h"

#define SMI_EINJ    0xE1        /* APM command: inject the error EINJ set */
#define SMI_PING    0xE0        /* APM command: answer in the mailbox */

void smm_reserve(void);         /* from arch_memory_reserve: TSEG out of the map */
void smm_init(void);            /* after smp_init and chipset_init */
int smm_einj_available(void);
u32 smm_hest_ghes(u8 *p, u16 source_id);   /* a 64-byte GHES entry, or 0 */
u8 *smm_einj_table(void *(*take)(u32 len));
void smm_lock(void);            /* D_LCK: before any image runs */
void smm_legacy_handoff(void);  /* before the CSM: locked, TSEG hidden from its RAM size */

#endif
