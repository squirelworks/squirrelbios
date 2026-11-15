/* SquirrelBIOS x86_64-uefi: the variable services kept in flash
 * (variables.c, runtime pages). Original code. */
#ifndef VARS_H
#define VARS_H

#include "efi.h"

/* vars_auth_hook checks a write before it is made (authvar.c): it may
 * replace the data with what follows an authentication descriptor and
 * set the time to keep. vars_after_hook runs after a write went through. */
typedef EFI_STATUS (*vars_auth_fn)(const CHAR16 *name, const EFI_GUID *vendor, u32 attr,
                                   const u8 **data, u32 *size, EFI_TIME *time,
                                   const EFI_TIME *old_time, int exited);
extern vars_auth_fn vars_auth_hook;
extern void (*vars_after_hook)(const CHAR16 *name, const EFI_GUID *vendor);

void vars_init(void);                           /* boot time */
void vars_install(EFI_RUNTIME_SERVICES *rt);    /* boot time */
EFI_STATUS EFIAPI vars_get(CHAR16 *name, EFI_GUID *vendor, u32 *attr, UINTN *size, void *data);
EFI_STATUS EFIAPI vars_next(UINTN *size, CHAR16 *name, EFI_GUID *vendor);
EFI_STATUS EFIAPI vars_set(CHAR16 *name, EFI_GUID *vendor, u32 attr, UINTN size, void *data);
EFI_STATUS EFIAPI vars_query(u32 attr, u64 *max, u64 *rem, u64 *maxsz);

#endif
