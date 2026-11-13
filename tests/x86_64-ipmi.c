/* What an OS finds of the BMC on x86_64 (roadmap F3). Original code,
 * written from the IPMI 2.0 specification (KCS, appendix C3 SPMI), SMBIOS
 * 3.x (types 38 and 42) and DMTF DSP0270. Built like tests/x86_64-einj.c.
 * Reports on COM1:
 *
 *   APP ipmi spmi=ok smbios38=ok kcs=ok redfish=ok
 *
 * spmi      the SPMI table is in the XSDT, sums to 0, and gives a KCS
 *           interface, IPMI 2.0, byte-wide system I/O at 0xCA2
 * smbios38  SMBIOS type 38: KCS, 2.0, I/O at 0xCA2 (0xCA3 with the I/O bit)
 * kcs       the interface is idle after the firmware, and Get Device ID
 *           and Get Device GUID answer when sent from here
 * redfish   SMBIOS type 42: a network host interface on the PCI NIC at
 *           00:<dev>.0 (its IDs), one Redfish over IP record, service
 *           169.254.0.1:443, the BMC's GUID as service UUID (zeros if
 *           it has none); "none" when
 *           there is no NIC and no type 42
 *
 * or "APP ipmi none" when nothing answers on the KCS ports and there is
 * neither an SPMI table nor a type 38; then returns 0. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 status;
typedef void *handle;
#define EFIAPI __attribute__((ms_abi))
typedef status (EFIAPI *fn)();

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } guid;
typedef struct { u64 signature; u32 revision, header_size, crc32, reserved; } table_header;
typedef struct {
    table_header hdr;
    fn raise_tpl, restore_tpl, allocate_pages, free_pages, get_memory_map, allocate_pool,
       free_pool, create_event, set_timer, wait_for_event, signal_event, close_event,
       check_event, install_protocol, reinstall_protocol, uninstall_protocol, handle_protocol,
       reserved, register_protocol_notify, locate_handle, locate_device_path,
       install_configuration_table, load_image, start_image, exit, unload_image,
       exit_boot_services, get_next_monotonic_count, stall, set_watchdog_timer,
       connect_controller, disconnect_controller, open_protocol, close_protocol,
       open_protocol_information, protocols_per_handle, locate_handle_buffer, locate_protocol;
} boot_services;
typedef struct { guid id; void *table; } config_table;
typedef struct {
    table_header hdr;
    void *vendor;
    u32 revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    void *con_out;
    handle stderr_handle;
    void *std_err;
    void *rt;
    boot_services *bs;
    u64 ntables;
    config_table *tables;
} system_table;

static void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static void putc(char c)
{
    while (!(inb(0x3FD) & 0x20))
        ;
    outb(0x3F8, (u8)c);
}

static void puts(const char *s)
{
    while (*s)
        putc(*s++);
}

static u16 cfg_read16(u8 dev, u8 off)
{
    outl(0xCF8, 0x80000000u | (u32)dev << 11 | (off & 0xFCu));
    return inw((u16)(0xCFC + (off & 2)));
}

static u8 cfg_read8(u8 dev, u8 off)
{
    outl(0xCF8, 0x80000000u | (u32)dev << 11 | (off & 0xFCu));
    return inb((u16)(0xCFC + (off & 3)));
}

static int same(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return rd16(p) | (u32)rd16(p + 2) << 16; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

static void *config(system_table *st, u32 d1, u16 d2, u16 d3)
{
    for (u64 i = 0; i < st->ntables; i++) {
        guid *g = &st->tables[i].id;
        if (g->d1 == d1 && g->d2 == d2 && g->d3 == d3)
            return st->tables[i].table;
    }
    return 0;
}

/* ---- KCS, as an OS driver would talk to it ---- */
#define KCS_DATA 0xCA2
#define KCS_ST   0xCA3

static int wait_st(u8 mask, u8 want)
{
    for (u32 i = 0; i < 10000000; i++)
        if ((inb(KCS_ST) & mask) == want)
            return 1;
    return 0;
}

static int kcs(const u8 *msg, u32 len, u8 *rsp, u32 cap)
{
    if (!wait_st(2, 0))
        return -1;
    outb(KCS_ST, 0x61);                 /* WRITE_START */
    for (u32 i = 0; i < len; i++) {
        if (!wait_st(2, 0) || (inb(KCS_ST) >> 6) != 2)
            return -1;
        if (inb(KCS_ST) & 1)
            (void)inb(KCS_DATA);
        if (i == len - 1) {
            outb(KCS_ST, 0x62);         /* WRITE_END */
            if (!wait_st(2, 0) || (inb(KCS_ST) >> 6) != 2)
                return -1;
            if (inb(KCS_ST) & 1)
                (void)inb(KCS_DATA);
        }
        outb(KCS_DATA, msg[i]);
    }
    u32 got = 0;
    for (;;) {
        if (!wait_st(2, 0))
            return -1;
        u8 state = inb(KCS_ST) >> 6;
        if (state == 0) {
            if (wait_st(1, 1))
                (void)inb(KCS_DATA);
            return (int)got;
        }
        if (state != 1 || !wait_st(1, 1) || got >= cap)
            return -1;
        rsp[got++] = inb(KCS_DATA);
        outb(KCS_DATA, 0x68);           /* READ */
    }
}

static const u8 *spmi_find(system_table *st)
{
    const u8 *rsdp = config(st, 0x8868e871, 0xe4f1, 0x11d3);
    if (!rsdp)
        return 0;
    const u8 *xsdt = (const u8 *)(unsigned long)rd64(rsdp + 24);
    u32 n = (rd32(xsdt + 4) - 36) / 8;
    for (u32 i = 0; i < n; i++) {
        const u8 *t = (const u8 *)(unsigned long)rd64(xsdt + 36 + 8 * i);
        if (same(t, "SPMI", 4))
            return t;
    }
    return 0;
}

/* The next structure after s: past its formatted area and strings */
static const u8 *smbios_next(const u8 *s)
{
    const u8 *p = s + s[1];
    while (p[0] || p[1])
        p++;
    return p + 2;
}

static const u8 *smbios_find(system_table *st, u8 type)
{
    const u8 *ep = config(st, 0xf2fd1544, 0x9794, 0x4a2c);
    if (!ep || !same(ep, "_SM3_", 5))
        return 0;
    const u8 *s = (const u8 *)(unsigned long)rd64(ep + 16), *end = s + rd32(ep + 12);
    while (s < end && s[0] != 127) {
        if (s[0] == type)
            return s;
        s = smbios_next(s);
    }
    return 0;
}

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    puts("APP ipmi");
    const u8 *spmi = spmi_find(st), *t38 = smbios_find(st, 38), *t42 = smbios_find(st, 42);
    if (inb(KCS_ST) == 0xFF && !spmi && !t38) {
        puts(t42 ? " none redfish=bad\r\n" : " none\r\n");
        return 0;
    }

    int ok = 0;
    if (spmi) {
        u8 sum = 0;
        for (u32 i = 0; i < rd32(spmi + 4); i++)
            sum = (u8)(sum + spmi[i]);
        ok = !sum && rd32(spmi + 4) >= 65 && spmi[8] == 5 && spmi[36] == 1 && spmi[37] == 1 &&
             rd16(spmi + 38) == 0x0200 && spmi[48] == 1 && spmi[49] == 8 &&
             rd64(spmi + 52) == KCS_DATA;
    }
    puts(ok ? " spmi=ok" : " spmi=bad");

    ok = t38 && t38[1] >= 0x12 && t38[4] == 1 && t38[5] == 0x20 && t38[6] == 0x20 &&
         rd64(t38 + 8) == (KCS_DATA | 1) && (t38[0x10] & 0xD0) == 0;
    puts(ok ? " smbios38=ok" : " smbios38=bad");

    u8 rsp[32], guid_rsp[32];
    const u8 get_id[2] = { 0x06 << 2, 0x01 }, get_guid[2] = { 0x06 << 2, 0x08 };
    int n = (inb(KCS_ST) >> 6) == 0 ? kcs(get_id, 2, rsp, sizeof rsp) : -1;
    int g = kcs(get_guid, 2, guid_rsp, sizeof guid_rsp);
    ok = n >= 14 && rsp[0] == (0x07 << 2) && rsp[1] == 0x01 && rsp[2] == 0 && rsp[7] == 0x02;
    puts(ok ? " kcs=ok" : " kcs=bad");

    if (!t42) {
        puts(" redfish=none\r\n");
        return 0;
    }
    /* the NIC the record names must be on bus 0 */
    const u8 *d = t42 + 6;
    int nic = 0;
    for (u8 dev = 0; dev < 32 && d[0] == 3; dev++)
        if (cfg_read16(dev, 0) == rd16(d + 1) && cfg_read16(dev, 2) == rd16(d + 3) &&
            cfg_read8(dev, 0x0B) == 0x02 && cfg_read16(dev, 0x2C) == rd16(d + 5) &&
            cfg_read16(dev, 0x2E) == rd16(d + 7))
            nic = 1;
    const u8 *r = t42 + 6 + t42[5];
    const u8 *p = r + 3;
    static const u8 svc[4] = { 169, 254, 0, 1 };
    static const u8 zero[16];
    int uuid = g == 19 && guid_rsp[2] == 0 ? same(p, guid_rsp + 3, 16) : same(p, zero, 16);
    ok = t42[4] == 0x40 && t42[5] == 9 && nic && r[0] == 1 && r[1] == 4 &&
         r[2] >= 91 && t42[1] == 6 + 9 + 3 + r[2] && uuid &&
         p[16] == 1 && p[17] == 1 && p[50] == 1 && p[51] == 1 && same(p + 52, svc, 4) &&
         rd16(p + 84) == 443 && p[90] == r[2] - 91;
    puts(ok ? " redfish=ok\r\n" : " redfish=bad\r\n");
    return 0;
}
