/* Host stubs that stand in for boot services when fuzzing common/efi
 * parsers (FAT, PE/COFF) outside the firmware. Original code. */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include "efi.h"

/* The Simple File System interface fat_install last installed, or NULL. */
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *host_last_sfs(void);

/* Free every outstanding pool/page allocation and clear protocol state. */
void host_reset(void);

#endif
