/* SquirrelBIOS x86_64-uefi: coreboot table hand-off (roadmap C5).
 * Original code. Written from the public coreboot table ABI described in
 * coreboot's "ABI data consumption" document and the BSD-licensed
 * coreboot_tables.h tag layout (signature "LBIO", tagged records,
 * CB_TAG_MEMORY / CB_TAG_FORWARD / CB_TAG_ACPI_RSDP / CB_TAG_SERIAL).
 * No coreboot or libpayload source was copied. */
#include "sq.h"

#ifndef CONFIG_COREBOOT
#define CONFIG_COREBOOT 0
#endif

/* Tags and memory types from the published ABI (not from coreboot code). */
#define CB_TAG_MEMORY     0x0001
#define CB_TAG_FORWARD    0x0011
#define CB_TAG_SERIAL     0x000f
#define CB_TAG_ACPI_RSDP  0x0043

#define CB_MEM_RAM        1
#define CB_MEM_RESERVED   2
#define CB_MEM_ACPI       3
#define CB_MEM_NVS        4
#define CB_MEM_UNUSABLE   5
#define CB_MEM_VENDOR     6
#define CB_MEM_TABLE      16

#define CB_SERIAL_IO      1

/* Scan the first megabyte for an LBIO header (16-byte aligned). */
#define CB_SCAN_END       0x100000u

static const void *cb_tables;
static const void *cb_acpi_rsdp;
static u16 cb_serial_base;
static int cb_have_mem;

/* Internet checksum (RFC 1071), as the ABI's header/table checksums use. */
static u16 ip_checksum(const void *p, u32 len)
{
    const u8 *b = p;
    u32 sum = 0;
    while (len > 1) {
        sum += (u32)b[0] | (u32)b[1] << 8;
        b += 2;
        len -= 2;
    }
    if (len)
        sum += *b;
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (u16)~sum;
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | (u64)rd32(p + 4) << 32;
}

/* A header is valid when the signature is LBIO and both checksums are 0. */
static int header_ok(const u8 *h)
{
    u32 hb, tb;
    if (memcmp(h, "LBIO", 4) != 0)
        return 0;
    hb = rd32(h + 4);
    tb = rd32(h + 12);
    if (hb < 24 || hb > 256 || tb > 0x100000u)
        return 0;
    if (ip_checksum(h, hb) != 0)
        return 0;
    /* table_checksum in the header is the RFC 1071 sum of the records */
    if (tb && ip_checksum(h + hb, tb) != (u16)rd32(h + 16))
        return 0;
    return 1;
}

static const u8 *follow(const u8 *h)
{
    u32 hb = rd32(h + 4);
    u32 tb = rd32(h + 12);
    u32 n = rd32(h + 20);
    const u8 *rec = h + hb;
    const u8 *end = rec + tb;
    while (n-- && rec + 8 <= end) {
        u32 tag = rd32(rec);
        u32 size = rd32(rec + 4);
        if (size < 8 || rec + size > end)
            break;
        if (tag == CB_TAG_FORWARD && size >= 16) {
            u64 fwd = rd64(rec + 8);
            if (fwd && fwd < 0x100000000ULL) {
                const u8 *next = (const u8 *)(uptr)fwd;
                if (header_ok(next))
                    return next;
            }
        }
        rec += size;
    }
    return h;
}

static const u8 *find_header(void)
{
    for (u32 off = 0; off < CB_SCAN_END; off += 16) {
        const u8 *h = (const u8 *)(uptr)off;
        if (!header_ok(h))
            continue;
        return follow(h);
    }
    return NULL;
}

static u32 e820_type(u32 cb)
{
    switch (cb) {
    case CB_MEM_RAM:      return 1;
    case CB_MEM_ACPI:     return 3;
    case CB_MEM_NVS:      return 4;
    case CB_MEM_RESERVED:
    case CB_MEM_UNUSABLE:
    case CB_MEM_VENDOR:
    case CB_MEM_TABLE:
    default:              return 2;
    }
}

static void parse_records(const u8 *h,
                          void (*on_mem)(u64, u64, u32, void *), void *mem_arg)
{
    u32 hb = rd32(h + 4);
    u32 tb = rd32(h + 12);
    u32 n = rd32(h + 20);
    const u8 *rec = h + hb;
    const u8 *end = rec + tb;
    while (n-- && rec + 8 <= end) {
        u32 tag = rd32(rec);
        u32 size = rd32(rec + 4);
        if (size < 8 || rec + size > end)
            break;
        if (tag == CB_TAG_MEMORY && on_mem) {
            const u8 *r = rec + 8;
            while (r + 20 <= rec + size) {
                u64 start = rd64(r);
                u64 len = rd64(r + 8);
                u32 type = rd32(r + 16);
                on_mem(start, len, e820_type(type), mem_arg);
                r += 20;
            }
        } else if (tag == CB_TAG_ACPI_RSDP && size >= 16) {
            u64 ptr = rd64(rec + 8);
            if (ptr)
                cb_acpi_rsdp = (const void *)(uptr)ptr;
        } else if (tag == CB_TAG_SERIAL && size >= 20) {
            u32 type = rd32(rec + 8);
            u32 base = rd32(rec + 12);
            if (type == CB_SERIAL_IO && base && base < 0x10000u)
                cb_serial_base = (u16)base;
        }
        rec += size;
    }
}

static void count_mem(u64 base, u64 len, u32 type, void *arg)
{
    int *n = arg;
    (void)base;
    (void)len;
    (void)type;
    (*n)++;
}

static void fill_mem(u64 base, u64 len, u32 type, void *arg)
{
    int *idx = arg;
    if (*idx >= MEM_MAX || !len)
        return;
    mem_map[*idx].base = base;
    mem_map[*idx].length = len;
    mem_map[*idx].type = type;
    (*idx)++;
}

int coreboot_present(void)
{
    return CONFIG_COREBOOT && cb_tables != NULL;
}

const void *coreboot_acpi_rsdp(void)
{
    return CONFIG_COREBOOT ? cb_acpi_rsdp : NULL;
}

u16 coreboot_serial_base(void)
{
    return CONFIG_COREBOOT ? cb_serial_base : 0;
}

int coreboot_memmap(void)
{
    int n = 0, i = 0;
    if (!CONFIG_COREBOOT)
        return 0;
    cb_tables = NULL;
    cb_acpi_rsdp = NULL;
    cb_serial_base = 0;
    cb_have_mem = 0;
    {
        const u8 *h = find_header();
        if (!h)
            return 0;
        parse_records(h, count_mem, &n);
        if (!n)
            return 0;
        mem_count = 0;
        parse_records(h, fill_mem, &i);
        mem_count = i;
        if (!i)
            return 0;
        cb_tables = h;
        cb_have_mem = 1;
        if (cb_serial_base)
            printf("coreboot: tables at %p, serial at 0x%x\n", h, cb_serial_base);
        else
            printf("coreboot: tables at %p\n", h);
        return 1;
    }
}
