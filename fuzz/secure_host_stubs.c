/* Host stubs for vars / fwcfg when linking x86_64/secure parsers under
 * the Authenticode fuzzer. Original code. FAT/PE harnesses do not need
 * these; keep them out of host_stubs.c. */
#include "secure.h"

vars_auth_fn vars_auth_hook;
void (*vars_after_hook)(const CHAR16 *name, const EFI_GUID *vendor);

EFI_STATUS EFIAPI vars_get(CHAR16 *name, EFI_GUID *vendor, u32 *attr, UINTN *size, void *data)
{
    (void)name;
    (void)vendor;
    (void)attr;
    (void)data;
    if (size)
        *size = 0;
    return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI vars_set(CHAR16 *name, EFI_GUID *vendor, u32 attr, UINTN size, void *data)
{
    (void)name;
    (void)vendor;
    (void)attr;
    (void)size;
    (void)data;
    return EFI_UNSUPPORTED;
}

int fwcfg_find(const char *name, u16 *select, u32 *size)
{
    (void)name;
    if (select)
        *select = 0;
    if (size)
        *size = 0;
    return 0;
}

void fwcfg_read(u16 select, void *buf, u32 len)
{
    (void)select;
    (void)buf;
    (void)len;
}
