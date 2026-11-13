/* SquirrelBIOS common/fw: formatted output on the firmware console.
 * Original code. */
#include "fwlib.h"

struct out {
    char *buf;
    size_t size, len;
};

static void emit(struct out *o, char c)
{
    if (o->buf) {
        if (o->len + 1 < o->size)
            o->buf[o->len] = c;
    } else {
        if (c == '\n')
            fw_putc('\r');
        fw_putc(c);
    }
    o->len++;
}

static void emit_num(struct out *o, uint64_t v, int base, int upper, int width,
                     char pad, int neg)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg && pad == '0')
        emit(o, '-');
    for (int i = n + neg; i < width; i++)
        emit(o, pad);
    if (neg && pad != '0')
        emit(o, '-');
    while (n)
        emit(o, tmp[--n]);
}

static int format(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0, left = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'c':
            emit(o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int n = (int)strlen(s);
            if (!left)
                for (int i = n; i < width; i++)
                    emit(o, ' ');
            while (*s)
                emit(o, *s++);
            if (left)
                for (int i = n; i < width; i++)
                    emit(o, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            emit_num(o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, 0, width, pad, v < 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            emit_num(o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, pad, 0);
            break;
        }
        case 'p':
            emit(o, '0');
            emit(o, 'x');
            emit_num(o, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 16, '0', 0);
            break;
        case '%':
            emit(o, '%');
            break;
        case 0:
            return (int)o->len;
        default:
            emit(o, '%');
            emit(o, *fmt);
        }
    }
    return (int)o->len;
}

int fw_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    int n = format(&o, fmt, ap);
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return n;
}

int fw_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fw_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int fw_printf(const char *fmt, ...)
{
    struct out o = { 0, 0, 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = format(&o, fmt, ap);
    va_end(ap);
    return n;
}

void fw_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            fw_putc('\r');
        fw_putc(*s);
    }
}
