/* SquirrelBIOS UEFI core (common/efi): serial console and a small printf.
 * Original code (from x86_64/core/console.c); the port itself is the
 * architecture's (arch_con_*).
 *
 * printf understands %s %c %d %u %x %X %p and %%, with an optional 0 flag,
 * a width, and l / ll (both 64-bit here). */
#include "sq.h"
#include <stdarg.h>

/* A second console beside the serial port (net_sol.c), when set */
void (*con_mirror_putc)(u8 c);
int (*con_mirror_getc)(void);

void con_putc(char c)
{
    if (c == '\n')
        con_raw('\r');
    con_raw(c);
}

/* A byte as it is, no newline translation */
void con_raw(char c)
{
    arch_con_putc((u8)c);
    if (con_mirror_putc)
        con_mirror_putc((u8)c);
}

/* The next received byte, or -1 */
int con_getc(void)
{
    int c = arch_con_getc();
    if (c < 0 && con_mirror_getc)
        c = con_mirror_getc();
    return c;
}

void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void put_num(u64 v, unsigned base, int upper, int width, char pad, int neg)
{
    char buf[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        buf[n++] = '-';
    while (n < width--)
        con_putc(pad);
    while (n)
        con_putc(buf[--n]);
}

void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            con_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            lng = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s ? s : "(null)");
            while (len < width--)
                con_putc(' ');
            con_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            con_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            s64 v = lng ? va_arg(ap, s64) : va_arg(ap, int);
            put_num(v < 0 ? (u64)-v : (u64)v, 10, 0, width, pad, v < 0);
            break;
        }
        case 'u':
            put_num(lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            put_num(lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, *fmt == 'X', width, pad, 0);
            break;
        case 'p':
            con_puts("0x");
            put_num((u64)(uptr)va_arg(ap, void *), 16, 0, 16, '0', 0);
            break;
        case '%':
            con_putc('%');
            break;
        default:
            con_putc('%');
            con_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}

void halt(void)
{
    arch_halt();
}
