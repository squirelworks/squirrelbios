/* SquirrelBIOS x86_64-uefi: SMBIOS 3.x tables (roadmap C2).
 * Original code, written from the DMTF SMBIOS Reference Specification
 * (DSP0134) 3.x: the 64-bit "_SM3_" entry point and structure types 0
 * (firmware), 1 (system), 3 (chassis), 4 (processor), 16/17/19
 * (memory array, device, mapped range), 32 (boot status), 38 (IPMI
 * device), 42 (Redfish host interface, DSP0270) and 127 (end).
 * The entry point is published as the SMBIOS 3 configuration table. */
#include "mgmt.h"

static const EFI_GUID smbios3_guid =
    { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } };

#define FW_CFG_UUID     0x0002
#define FW_CFG_RAM_SIZE 0x0003
#define TABLE_PAGES     2
#define SQ_RELEASE_DATE "11/13/2026"

static u8 *buf, *end, *cur;
static u16 next_handle;
static int full;

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

/* Start a structure: zeroed formatted area of len bytes with its header */
static u8 *begin(u8 type, u8 len, u16 *handle)
{
    if (cur + len + 64 > end) {
        full = 1;
        return NULL;
    }
    u8 *s = cur;
    memset(s, 0, len);
    s[0] = type;
    s[1] = len;
    put16(s + 2, next_handle);
    if (handle)
        *handle = next_handle;
    next_handle++;
    cur += len;
    return s;
}

/* The string set: strings in order (numbered from 1), then a NUL. An
 * empty set is two NULs. */
static void strings(const char *const *s, int n)
{
    int any = 0;
    for (int i = 0; i < n; i++) {
        usize l = strlen(s[i]);
        if (!l)
            continue;
        if (cur + l + 2 > end) {
            full = 1;
            return;
        }
        memcpy(cur, s[i], l + 1);
        cur += l + 1;
        any = 1;
    }
    if (!any)
        *cur++ = 0;
    *cur++ = 0;
}

static void type0(void)
{
    u8 *s = begin(0, 0x1A, NULL);
    if (!s)
        return;
    s[4] = 1;                           /* vendor */
    s[5] = 2;                           /* version */
    put16(s + 6, 0);                    /* no legacy segment: UEFI */
    s[8] = 3;                           /* release date */
    s[9] = 0x0F;                        /* ROM: 16 x 64 KB = 1 MB */
    put64(s + 10, 1u << 7);             /* PCI */
    s[0x12] = 0x01;                     /* ACPI */
    s[0x13] = 0x08 | 0x10;              /* UEFI, virtual machine */
    s[0x14] = 2;                        /* release 2.0 */
    s[0x15] = 0;
    s[0x16] = 0xFF;                     /* no embedded controller */
    s[0x17] = 0xFF;
    const char *str[] = { "SquirrelWorks", "SquirrelBIOS " SQ_VERSION, SQ_RELEASE_DATE };
    strings(str, 3);
}

static void type1(int q35)
{
    u8 *s = begin(1, 0x1B, NULL);
    if (!s)
        return;
    s[4] = 1;
    s[5] = 2;
    s[6] = 3;
    u8 uuid[16] = { 0 };
    if (fwcfg_present())
        fwcfg_read(FW_CFG_UUID, uuid, 16);
    /* SMBIOS stores the first three UUID fields little-endian */
    u8 *u = s + 8;
    u[0] = uuid[3]; u[1] = uuid[2]; u[2] = uuid[1]; u[3] = uuid[0];
    u[4] = uuid[5]; u[5] = uuid[4];
    u[6] = uuid[7]; u[7] = uuid[6];
    memcpy(u + 8, uuid + 8, 8);
    s[0x18] = 6;                        /* woken by the power switch */
    s[0x1A] = 4;                        /* family */
    const char *str[] = { "SquirrelWorks", q35 ? "Virtual PC (q35)" : "Virtual PC (i440fx)",
                          SQ_VERSION, "Squirrel" };
    strings(str, 4);
}

static void type3(void)
{
    u8 *s = begin(3, 0x16, NULL);
    if (!s)
        return;
    s[4] = 1;
    s[5] = 0x01;                        /* other */
    s[9] = 3;                           /* boot-up, power, thermal: safe */
    s[10] = 3;
    s[11] = 3;
    s[12] = 3;                          /* security: none */
    const char *str[] = { "SquirrelWorks" };
    strings(str, 1);
}

static void type4(u32 index)
{
    u32 a, b, c, d;
    u8 *s = begin(4, 0x30, NULL);
    if (!s)
        return;
    s[4] = 1;                           /* socket designation */
    s[5] = 3;                           /* central processor */
    s[6] = 0x01;                        /* family: other */
    s[7] = 2;                           /* manufacturer */
    cpuid(1, &a, &b, &c, &d);
    put32(s + 8, a);                    /* processor ID: CPUID 1 EAX, EDX */
    put32(s + 12, d);
    s[0x10] = 3;                        /* version: the brand string */
    put16(s + 0x14, 2000);              /* max speed, MHz (nominal) */
    put16(s + 0x16, 2000);
    s[0x18] = 0x41;                     /* populated, enabled */
    s[0x19] = 0x01;                     /* upgrade: other */
    put16(s + 0x1A, 0xFFFF);            /* no cache structures */
    put16(s + 0x1C, 0xFFFF);
    put16(s + 0x1E, 0xFFFF);
    s[0x23] = 1;                        /* cores, enabled, threads */
    s[0x24] = 1;
    s[0x25] = 1;
    put16(s + 0x26, 0x0004);            /* 64-bit capable */
    put16(s + 0x28, 0x0001);
    put16(s + 0x2A, 1);
    put16(s + 0x2C, 1);
    put16(s + 0x2E, 1);

    char vendor[13], brand[49], socket[8] = "CPU ";
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    brand[0] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        u32 *w = (u32 *)brand;
        for (u32 i = 0; i < 3; i++)
            cpuid(0x80000002 + i, &w[4 * i], &w[4 * i + 1], &w[4 * i + 2], &w[4 * i + 3]);
        brand[48] = 0;
    }
    const char *bp = brand;
    while (*bp == ' ')
        bp++;
    char digits[4];
    int nd = 0;
    u32 v = index;
    do {
        digits[nd++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && nd < 3);
    for (int i = 0; i < nd; i++)
        socket[4 + i] = digits[nd - 1 - i];
    socket[4 + nd] = 0;
    const char *str[] = { socket, vendor, *bp ? bp : "x86-64 processor" };
    strings(str, 3);
}

static void memory(u64 ram)
{
    u16 array;
    u8 *s = begin(16, 0x17, &array);
    if (!s)
        return;
    s[4] = 0x03;                        /* system board */
    s[5] = 0x03;                        /* system memory */
    s[6] = 0x03;                        /* no error correction */
    u64 kb = ram >> 10;
    if (kb < 0x80000000ull) {
        put32(s + 7, (u32)kb);
    } else {
        put32(s + 7, 0x80000000u);
        put64(s + 0x0F, ram);
    }
    put16(s + 0x0B, 0xFFFE);            /* no error information */
    put16(s + 0x0D, 1);
    strings(NULL, 0);

    s = begin(17, 0x28, NULL);
    if (!s)
        return;
    put16(s + 4, array);
    put16(s + 6, 0xFFFE);
    put16(s + 8, 64);
    put16(s + 10, 64);
    u64 mb = ram >> 20;
    if (mb < 0x7FFF) {
        put16(s + 12, (u16)mb);
    } else {
        put16(s + 12, 0x7FFF);
        put32(s + 0x1C, (u32)mb);
    }
    s[0x0E] = 0x09;                     /* DIMM */
    s[0x10] = 1;                        /* device locator */
    s[0x11] = 2;                        /* bank locator */
    s[0x12] = 0x07;                     /* RAM */
    put16(s + 0x13, 0x0002);            /* unknown detail */
    s[0x17] = 3;                        /* manufacturer */
    const char *str[] = { "DIMM 0", "Bank 0", "SquirrelWorks" };
    strings(str, 3);

    for (int i = 0; i < mem_count; i++) {
        if (mem_map[i].type != 1)
            continue;
        u64 start = mem_map[i].base, stop = mem_map[i].base + mem_map[i].length - 1;
        s = begin(19, 0x1F, NULL);
        if (!s)
            return;
        if (stop >> 10 < 0xFFFFFFFFull) {
            put32(s + 4, (u32)(start >> 10));
            put32(s + 8, (u32)(stop >> 10));
        } else {
            put32(s + 4, 0xFFFFFFFFu);
            put32(s + 8, 0xFFFFFFFFu);
            put64(s + 0x0F, start);
            put64(s + 0x17, stop);
        }
        put16(s + 0x0C, array);
        s[0x0E] = 1;
        strings(NULL, 0);
    }
}

/* Type 38: the BMC on KCS, byte-spaced I/O ports, no interrupt */
static void type38(void)
{
    u8 *s = begin(38, 0x12, NULL);
    if (!s)
        return;
    s[4] = 1;                           /* KCS */
    s[5] = ipmi_version();
    s[6] = 0x20;                        /* the BMC's I2C target address */
    s[7] = 0xFF;                        /* no NV storage device */
    put64(s + 8, IPMI_KCS_BASE | 1);    /* bit 0: I/O space */
    s[0x10] = 0x00;                     /* byte boundaries, address bit 0 is 0 */
    s[0x11] = 0;                        /* no interrupt */
    strings(NULL, 0);
}

/* Type 42: a network host interface (40h) on a PCI NIC (descriptor
 * type 03h), with one Redfish over IP protocol record (04h) */
static void type42(const struct redfish_hi *hi)
{
    u8 name_len = (u8)strlen(hi->hostname);
    u8 *s = begin(42, (u8)(6 + 9 + 1 + 2 + 91 + name_len), NULL);
    if (!s)
        return;
    s[4] = 0x40;
    s[5] = 9;
    u8 *d = s + 6;
    d[0] = 0x03;
    put16(d + 1, hi->vendor);
    put16(d + 3, hi->device);
    put16(d + 5, hi->sub_vendor);
    put16(d + 7, hi->sub_device);
    d += 9;
    *d++ = 1;                           /* one protocol record */
    *d++ = 0x04;                        /* Redfish over IP */
    *d++ = (u8)(91 + name_len);
    memcpy(d, hi->uuid, 16);
    d[16] = 1;                          /* host address: static */
    d[17] = 1;                          /* IPv4 */
    memcpy(d + 18, hi->host_ip, 4);
    memcpy(d + 34, hi->mask, 4);
    d[50] = 1;                          /* service address: static */
    d[51] = 1;
    memcpy(d + 52, hi->service_ip, 4);
    memcpy(d + 68, hi->mask, 4);
    put16(d + 84, hi->port);
    put32(d + 86, 0);                   /* no VLAN */
    d[90] = name_len;
    memcpy(d + 91, hi->hostname, name_len);
    strings(NULL, 0);
}

void smbios_init(void)
{
    EFI_PHYSICAL_ADDRESS base = 0;
    if (efi_allocate_pages(AllocateAnyPages, EfiACPIReclaimMemory, TABLE_PAGES, &base)) {
        printf("smbios: no memory for the tables\n");
        return;
    }
    u8 *ep = (u8 *)(uptr)base;
    buf = cur = ep + 32;
    end = ep + TABLE_PAGES * 4096;
    next_handle = 0;
    full = 0;

    u64 ram = 0;
    if (fwcfg_present())
        fwcfg_read(FW_CFG_RAM_SIZE, &ram, 8);
    if (!ram)
        for (int i = 0; i < mem_count; i++)
            if (mem_map[i].type == 1)
                ram += mem_map[i].length;
    u8 ncpu[2] = { 1, 0 };
    if (fwcfg_present())
        fwcfg_read(0x0005, ncpu, 2);
    u32 cpus = ncpu[0] | (u32)ncpu[1] << 8;
    if (!cpus)
        cpus = 1;

    type0();
    type1(chipset()->q35);
    type3();
    for (u32 i = 0; i < cpus && !full; i++)
        type4(i);
    memory(ram);
    struct redfish_hi hi;
    if (ipmi_present())
        type38();
    if (redfish_host_interface(&hi))
        type42(&hi);
    u8 *s = begin(32, 0x0B, NULL);
    if (s)
        strings(NULL, 0);
    s = begin(127, 4, NULL);
    if (s)
        strings(NULL, 0);
    if (full) {
        printf("smbios: tables do not fit\n");
        return;
    }

    /* 64-bit entry point */
    memset(ep, 0, 24);
    memcpy(ep, "_SM3_", 5);
    ep[6] = 0x18;
    ep[7] = 3;                          /* SMBIOS 3.0 */
    ep[8] = 0;
    ep[9] = 0;
    ep[10] = 1;                         /* entry point revision */
    put32(ep + 12, (u32)(cur - buf));
    put64(ep + 16, (u64)(uptr)buf);
    ep[5] = acpi_checksum(ep, 24);

    efi_install_configuration_table((EFI_GUID *)&smbios3_guid, ep);
    printf("smbios: 3.0 entry point at %p, %u structures, %u bytes\n", ep, next_handle,
           (u32)(cur - buf));
}
