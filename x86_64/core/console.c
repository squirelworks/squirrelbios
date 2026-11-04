/* SquirrelBIOS x86_64-uefi: serial console and a small printf.
 * Original code.
 *
 * printf understands %s %c %d %u %x %X %p and %%, with an optional 0 flag,
 * a width, and l / ll (both 64-bit here). */
#include "sq.h"
#include <stdarg.h>

#define COM1 CONFIG_COM1_BASE

void con_init(void)
{
    outb(COM1 + 1, 0x00);       /* no interrupts */
    outb(COM1 + 3, 0x80);       /* DLAB */
    outb(COM1 + 0, 0x01);       /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8N1 */
    outb(COM1 + 2, 0xC7);       /* FIFOs on, cleared */
    outb(COM1 + 4, 0x03);       /* DTR, RTS */
}

void con_putc(char c)
{
    if (c == '\n')
        con_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, (u8)c);
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
    for (;;)
        __asm__ volatile("cli; hlt");
}
