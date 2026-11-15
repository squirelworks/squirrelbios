/* SquirrelBIOS UEFI core (common/efi): the few C library routines the firmware uses.
 * Original code. */
#include "efi.h"

void *memset(void *d, int c, usize n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, usize n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (u8)*a - (u8)*b;
}

void *memmove(void *d, const void *s, usize n)
{
    u8 *p = d;
    const u8 *q = s;
    if (p < q || p >= q + n)
        while (n--)
            *p++ = *q++;
    else
        while (n--)
            p[n] = q[n];
    return d;
}

int strncmp(const char *a, const char *b, usize n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (u8)*a - (u8)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

/* The CRC-32 of UEFI table headers. It lives here, with the runtime
 * code, because SetVirtualAddressMap reseals tables after
 * ExitBootServices. */
u32 crc32(const void *buf, usize len)
{
    const u8 *p = buf;
    u32 c = 0xFFFFFFFF;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

void efi_reseal(EFI_TABLE_HEADER *h)
{
    h->crc32 = 0;
    h->crc32 = crc32(h, h->header_size);
}
