/* Stand-in OS for CXL Early Discovery on x86-64 (roadmap F2).
 * Original code, written from the CXL 2.0 / 3.0 CEDT (CHBS, CFMWS) and
 * the ACPI 6.5 XSDT. Built as a static ELF turned into a PE by
 * tests/mkpe.py. It walks the XSDT for CEDT, checks one CHBS and one
 * CFMWS against the ranges QEMU assigns for the smoke topology
 * (-m 512M, bus_nr=52, cxl-fmw size 4G), looks for ACPI0016/ACPI0017 in
 * the DSDT, and reports on COM1:
 *
 *   APP cxl cedt=ok chbs=ok cfmws=ok dsdt=ok cxl=ok
 *
 * or "APP cxl none" when there is no CEDT (no cxl=on / no pxb-cxl).
 * then returns 0. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long uptr;
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

/* QEMU q35,cxl=on with -m 512M, pxb-cxl bus_nr=52, cxl-fmw.0.size=4G:
 * host_mr at 4 GiB (1 MiB), CFMW rounded up to 256 MiB at 0x110000000. */
#define EXP_UID         52u
#define EXP_CHBS_BASE   0x100000000ull
#define EXP_CHBS_LEN    0x10000ull
#define EXP_CFMWS_BASE  0x110000000ull
#define EXP_CFMWS_SIZE  0x100000000ull

static const guid acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

static void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

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

static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }

static u8 checksum(const u8 *p, u32 len)
{
    u8 s = 0;
    while (len--)
        s = (u8)(s + *p++);
    return s;
}

static int memeq(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static u8 *find_rsdp(system_table *st)
{
    for (u64 i = 0; i < st->ntables; i++) {
        if (st->tables[i].id.d1 != acpi20.d1)
            continue;
        u8 *rsdp = st->tables[i].table;
        if (rsdp && memeq(rsdp, "RSD PTR ", 8) && !checksum(rsdp, 20) &&
            (rsdp[15] < 2 || !checksum(rsdp, get32(rsdp + 20))))
            return rsdp;
    }
    return 0;
}

static u8 *find_xsdt_table(u8 *xsdt, const char *sig)
{
    if (!xsdt || !memeq(xsdt, "XSDT", 4) || checksum(xsdt, get32(xsdt + 4)))
        return 0;
    u32 xlen = get32(xsdt + 4);
    for (u32 at = 36; at + 8 <= xlen; at += 8) {
        u8 *t = (u8 *)(uptr)get64(xsdt + at);
        if (t && memeq(t, sig, 4) && !checksum(t, get32(t + 4)))
            return t;
    }
    return 0;
}

/* DSDT is named by the FADT (X_DSDT), not listed in the XSDT */
static u8 *find_dsdt(u8 *xsdt)
{
    u8 *fadt = find_xsdt_table(xsdt, "FACP");
    if (!fadt)
        return 0;
    u32 flen = get32(fadt + 4);
    u8 *dsdt = 0;
    if (flen >= 148)
        dsdt = (u8 *)(uptr)get64(fadt + 140);       /* X_DSDT */
    if ((!dsdt || !memeq(dsdt, "DSDT", 4)) && flen >= 44)
        dsdt = (u8 *)(uptr)(u32)get32(fadt + 40);   /* DSDT */
    if (dsdt && memeq(dsdt, "DSDT", 4) && !checksum(dsdt, get32(dsdt + 4)))
        return dsdt;
    return 0;
}

static int find_str(const u8 *p, u32 len, const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    if (!n || len < n)
        return 0;
    for (u32 i = 0; i + n <= len; i++) {
        u32 j = 0;
        while (j < n && p[i + j] == (u8)s[j])
            j++;
        if (j == n)
            return 1;
    }
    return 0;
}

status EFIAPI efi_main(handle image, system_table *st)
{
    (void)image;
    puts("APP cxl");
    u8 *rsdp = find_rsdp(st);
    u8 *xsdt = rsdp ? (u8 *)(uptr)get64(rsdp + 24) : 0;
    u8 *cedt = find_xsdt_table(xsdt, "CEDT");
    if (!cedt) {
        puts(" none\n");
        return 0;
    }
    u32 len = get32(cedt + 4);
    int chbs_ok = 0, cfmws_ok = 0, nchbs = 0, ncfmws = 0;
    for (u32 at = 36; at + 4 <= len;) {
        u8 type = cedt[at];
        u16 rlen = get16(cedt + at + 2);
        if (rlen < 4 || at + rlen > len)
            break;
        if (type == 0) {
            nchbs++;
            if (rlen >= 32 && get32(cedt + at + 4) == EXP_UID &&
                get64(cedt + at + 16) == EXP_CHBS_BASE &&
                get64(cedt + at + 24) == EXP_CHBS_LEN)
                chbs_ok = 1;
        } else if (type == 1) {
            ncfmws++;
            if (rlen >= 40 && get64(cedt + at + 8) == EXP_CFMWS_BASE &&
                get64(cedt + at + 16) == EXP_CFMWS_SIZE &&
                get32(cedt + at + 36) == EXP_UID)
                cfmws_ok = 1;
        }
        at += rlen;
    }
    u8 *dsdt = find_dsdt(xsdt);
    int dsdt_ok = 0;
    if (dsdt) {
        u32 dlen = get32(dsdt + 4);
        if (dlen > 36 && find_str(dsdt + 36, dlen - 36, "ACPI0016") &&
            find_str(dsdt + 36, dlen - 36, "ACPI0017"))
            dsdt_ok = 1;
    }

    puts(" cedt=");
    puts(nchbs == 1 && ncfmws >= 1 ? "ok" : "bad");
    puts(" chbs=");
    puts(chbs_ok ? "ok" : "bad");
    puts(" cfmws=");
    puts(cfmws_ok ? "ok" : "bad");
    puts(" dsdt=");
    puts(dsdt_ok ? "ok" : "bad");
    if (nchbs == 1 && ncfmws >= 1 && chbs_ok && cfmws_ok && dsdt_ok)
        puts(" cxl=ok\n");
    else
        puts(" =bad\n");
    return 0;
}
