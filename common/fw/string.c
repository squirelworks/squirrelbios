/* SquirrelBIOS common/fw: memory and string helpers.
 * Original code. */
#include "fwlib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

int fw_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int base = 10, digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++, digits++) {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return -1;
        v = v * base + d;
    }
    if (!digits)
        return -1;
    *out = v;
    return 0;
}

uint32_t fw_get_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t fw_get_be64(const void *p)
{
    const uint8_t *b = p;
    return (uint64_t)fw_get_be32(b) << 32 | fw_get_be32(b + 4);
}

void fw_put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

void fw_put_be64(void *p, uint64_t v)
{
    fw_put_be32(p, v >> 32);
    fw_put_be32((uint8_t *)p + 4, (uint32_t)v);
}
