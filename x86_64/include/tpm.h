/* SquirrelBIOS x86_64-uefi: measured boot with a TPM 2.0 (roadmap C5).
 * Original code. */
#ifndef TPM_H
#define TPM_H

#include "efi.h"

#define TPM_TIS_BASE 0xFED40000u
#define TPM_TIS_SIZE 0x5000u

/* tis.c: the FIFO interface, locality 0 */
int tis_probe(u32 *did_vid);
u32 tis_command(const u8 *in, u32 len, u8 *out, u32 cap);

/* tcg2.c: PCR extends, the event log, EFI_TCG2_PROTOCOL, the TPM2 table */
void tpm_init(void);                    /* before acpi_init */
int tpm_present(void);
void tpm_measure_image(const u8 *img, UINTN size);
int tpm_acpi_tables(u8 **out, void *(*take)(u32 len));

#endif
