/* SquirrelBIOS x86_64-uefi: a small AML (ACPI Machine Language) writer.
 * Original code, written from the ACPI 6.5 specification, chapter 20
 * (AML grammar) and section 6.4 (resource data types).
 *
 * The DSDT is generated at boot instead of compiled from ASL, so it can
 * follow the machine (q35 or i440fx, HPET present or not). Objects with a
 * PkgLength are written with aml_open() / aml_close(): close measures
 * what was written since the open and inserts the encoded length in
 * front of it. */
#include "acpi.h"

void aml_init(struct aml *a, u8 *buf, u32 cap)
{
    a->buf = buf;
    a->cap = cap;
    a->len = 0;
    a->depth = 0;
    a->overflow = 0;
}

void aml_byte(struct aml *a, u8 b)
{
    if (a->len >= a->cap) {
        a->overflow = 1;
        return;
    }
    a->buf[a->len++] = b;
}

void aml_bytes(struct aml *a, const void *p, u32 n)
{
    const u8 *b = p;
    while (n--)
        aml_byte(a, *b++);
}

void aml_u16(struct aml *a, u16 v) { aml_byte(a, (u8)v); aml_byte(a, (u8)(v >> 8)); }
void aml_u32(struct aml *a, u32 v) { aml_u16(a, (u16)v); aml_u16(a, (u16)(v >> 16)); }

/* PkgLength (20.2.4): one byte below 64, else a lead byte holding the
 * count of following bytes in bits 7:6 and the low nibble of the length.
 * The length counts the PkgLength bytes themselves. */
void aml_open(struct aml *a)
{
    if (a->depth >= AML_DEPTH) {
        a->overflow = 1;
        return;
    }
    a->stack[a->depth++] = a->len;
}

void aml_close(struct aml *a)
{
    if (!a->depth) {
        a->overflow = 1;
        return;
    }
    u32 start = a->stack[--a->depth];
    u32 body = a->len - start;
    u8 enc[4];
    u32 n;
    if (body + 1 < 0x40) {
        enc[0] = (u8)(body + 1);
        n = 1;
    } else {
        u32 total;
        n = body + 2 < 0x1000 ? 2 : body + 3 < 0x100000 ? 3 : 4;
        total = body + n;
        enc[0] = (u8)(((n - 1) << 6) | (total & 0x0F));
        for (u32 i = 1; i < n; i++)
            enc[i] = (u8)(total >> (4 + 8 * (i - 1)));
    }
    if (a->len + n > a->cap) {
        a->overflow = 1;
        return;
    }
    for (u32 i = a->len; i > start; i--)
        a->buf[i - 1 + n] = a->buf[i - 1];
    for (u32 i = 0; i < n; i++)
        a->buf[start + i] = enc[i];
    a->len += n;
}

/* NameString (20.2.2) from "\\_SB.PCI0.LNKA"-style text: an optional
 * root '\' or parent '^' prefixes, then dotted segments of up to four
 * characters, padded with '_'. */
void aml_name(struct aml *a, const char *path)
{
    while (*path == '\\' || *path == '^')
        aml_byte(a, (u8)*path++);
    int segs = 0;
    for (const char *p = path; *p; p++)
        if (*p == '.')
            segs++;
    segs++;
    if (!*path) {
        aml_byte(a, 0x00);              /* NullName */
        return;
    }
    if (segs == 2)
        aml_byte(a, 0x2E);              /* DualNamePrefix */
    else if (segs > 2) {
        aml_byte(a, 0x2F);              /* MultiNamePrefix */
        aml_byte(a, (u8)segs);
    }
    while (*path) {
        int i = 0;
        while (*path && *path != '.') {
            if (i < 4)
                aml_byte(a, (u8)*path);
            i++;
            path++;
        }
        for (; i < 4; i++)
            aml_byte(a, '_');
        if (*path == '.')
            path++;
    }
}

void aml_int(struct aml *a, u64 v)
{
    if (v == 0)
        aml_byte(a, 0x00);              /* ZeroOp */
    else if (v == 1)
        aml_byte(a, 0x01);              /* OneOp */
    else if (v <= 0xFF) {
        aml_byte(a, 0x0A);
        aml_byte(a, (u8)v);
    } else if (v <= 0xFFFF) {
        aml_byte(a, 0x0B);
        aml_u16(a, (u16)v);
    } else if (v <= 0xFFFFFFFFull) {
        aml_byte(a, 0x0C);
        aml_u32(a, (u32)v);
    } else {
        aml_byte(a, 0x0E);
        aml_u32(a, (u32)v);
        aml_u32(a, (u32)(v >> 32));
    }
}

void aml_string(struct aml *a, const char *s)
{
    aml_byte(a, 0x0D);
    aml_bytes(a, s, (u32)strlen(s) + 1);
}

/* EISA ID: three letters in 5 bits each, then four hex digits, stored
 * as a big-endian 32-bit value. */
u32 aml_eisaid(const char *id)
{
    static const char hex[] = "0123456789ABCDEF";
    u32 v = (u32)(id[0] & 0x1F) << 26 | (u32)(id[1] & 0x1F) << 21 | (u32)(id[2] & 0x1F) << 16;
    for (int i = 0; i < 4; i++) {
        u32 d = 0;
        while (hex[d] && hex[d] != id[3 + i])
            d++;
        v |= (d & 0xF) << (12 - 4 * i);
    }
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

void aml_name_int(struct aml *a, const char *name, u64 v)
{
    aml_byte(a, 0x08);                  /* NameOp */
    aml_name(a, name);
    aml_int(a, v);
}

void aml_name_eisaid(struct aml *a, const char *name, const char *id)
{
    aml_byte(a, 0x08);
    aml_name(a, name);
    aml_byte(a, 0x0C);
    aml_u32(a, aml_eisaid(id));
}

void aml_scope(struct aml *a, const char *name)
{
    aml_byte(a, 0x10);                  /* ScopeOp */
    aml_open(a);
    aml_name(a, name);
}

void aml_device(struct aml *a, const char *name)
{
    aml_byte(a, 0x5B);                  /* ExtOpPrefix */
    aml_byte(a, 0x82);                  /* DeviceOp */
    aml_open(a);
    aml_name(a, name);
}

void aml_method(struct aml *a, const char *name, u8 args)
{
    aml_byte(a, 0x14);                  /* MethodOp */
    aml_open(a);
    aml_name(a, name);
    aml_byte(a, args & 7);              /* not serialized, sync level 0 */
}

void aml_package(struct aml *a, u8 count)
{
    aml_byte(a, 0x12);                  /* PackageOp */
    aml_open(a);
    aml_byte(a, count);
}

void aml_end(struct aml *a)
{
    aml_close(a);
}

/* Name(name, Buffer(){resource template}) for a template already built
 * in rt[0..len), with its End Tag appended here. */
void aml_name_resources(struct aml *a, const char *name, const struct res *r)
{
    aml_byte(a, 0x08);
    aml_name(a, name);
    aml_byte(a, 0x11);                  /* BufferOp */
    aml_open(a);
    aml_int(a, r->len + 2);
    aml_bytes(a, r->buf, r->len);
    aml_byte(a, 0x79);                  /* End Tag, checksum 0 = ignore */
    aml_byte(a, 0x00);
    aml_close(a);
}

/* Resource descriptors (6.4) */

static void rb(struct res *r, u8 b)
{
    if (r->len < sizeof r->buf)
        r->buf[r->len++] = b;
}

static void rw(struct res *r, u16 v) { rb(r, (u8)v); rb(r, (u8)(v >> 8)); }
static void rd(struct res *r, u32 v) { rw(r, (u16)v); rw(r, (u16)(v >> 16)); }

void res_io(struct res *r, u16 base, u8 len)
{
    rb(r, 0x47);                        /* I/O port, 16-bit decode */
    rb(r, 0x01);
    rw(r, base);
    rw(r, base);
    rb(r, 1);
    rb(r, len);
}

void res_irq(struct res *r, u8 irq)
{
    rb(r, 0x22);                        /* IRQ, edge, active high */
    rw(r, (u16)(1u << irq));
}

void res_mem32_fixed(struct res *r, u32 base, u32 len, int writable)
{
    rb(r, 0x86);
    rw(r, 9);
    rb(r, writable ? 1 : 0);
    rd(r, base);
    rd(r, len);
}

/* Interrupt(ResourceConsumer, Level, ActiveHigh, Shared) { gsi } */
void res_interrupt_level_high(struct res *r, u32 gsi)
{
    rb(r, 0x89);
    rw(r, 6);
    rb(r, 0x01 | 0x08);                 /* consumer, level, high, shared */
    rb(r, 1);
    rd(r, gsi);
}

/* Word address space: bus numbers (type 2) or I/O (type 1), producer,
 * fixed min and max. */
void res_word(struct res *r, u8 type, u16 min, u16 max)
{
    rb(r, 0x88);
    rw(r, 13);
    rb(r, type);
    rb(r, 0x0C);                        /* producer, MinFixed, MaxFixed */
    rb(r, type == 1 ? 0x03 : 0x00);     /* I/O: entire range */
    rw(r, 0);                           /* granularity */
    rw(r, min);
    rw(r, max);
    rw(r, 0);                           /* translation */
    rw(r, (u16)(max - min + 1));
}

/* DWord memory window, producer, fixed, read/write */
void res_dword_mem(struct res *r, u32 min, u32 max, int cacheable)
{
    rb(r, 0x87);
    rw(r, 23);
    rb(r, 0);                           /* memory range */
    rb(r, 0x0C);
    rb(r, (u8)(0x01 | (cacheable ? 0x02 : 0x00)));
    rd(r, 0);
    rd(r, min);
    rd(r, max);
    rd(r, 0);
    rd(r, max - min + 1);
}
