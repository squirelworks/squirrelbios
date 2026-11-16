/* Pre-3.0 verified-chain rehearsal stand-in (Phase G M18 / G-m3-rehearsal).
 * Original code. Combines the M15 Secure Boot everyday path with the M5/M16
 * SQTL event-log hand-off under one QEMU recipe: project-signed PE, SB on
 * (fw_cfg db), TPM present, stand-in confirms SQTL + GetEventLog agree,
 * extends PCR 8, then ExitBootServices still sees the log. Firmware also
 * prints measured-boot policy v1 (M16).
 *
 * Reports on COM1 (needs swtpm + SB db + signed image):
 *
 *   APP m3-rehearsal cfg=ok acpi=ok log=ok tcg2=ok extend=ok
 *   … ExitBootServices …
 *   APP m3-rehearsal ebs=ok
 *
 * then writes 0 to isa-debug-exit at 0xF4 (QEMU exit status 1). */

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

#define SQTL_MAGIC      0x4C545153u
#define SQTL_VERSION    1u
#define SQTL_FORMAT     2u
#define SQTL_F_PRESENT  1u
#define TCG2_BOOT_HASH_ALG_SHA256  0x00000002u

typedef struct {
    u32 Magic, Version, Format, Flags;
    u64 LogBase, LogSize, LogLast, FinalEvents;
    u32 FinalEventsSize, ActiveBanks;   /* M16: was Reserved */
} sqtl_t;

typedef status (EFIAPI *tcg2_get_event_log_fn)(void *this, u32 format, u64 *where,
                                               u64 *last, u8 *truncated);
typedef status (EFIAPI *tcg2_hash_log_extend_fn)(void *this, u64 flags, u64 data,
                                                 u64 len, void *event);

typedef struct {
    void *get_capability;
    tcg2_get_event_log_fn get_event_log;
    tcg2_hash_log_extend_fn hash_log_extend_event;
} tcg2_proto;

/* EFI_TCG2_EVENT: Size, HeaderSize, HeaderVersion, PCRIndex, EventType, data.
 * Packed — HeaderVersion is u16 with no pad before PCRIndex (matches the
 * asm stand-ins and the firmware HashLogExtendEvent parser). */
typedef struct __attribute__((packed)) {
    u32 size;
    u32 header_size;
    u16 header_version;
    u32 pcr_index;
    u32 event_type;
    u8 data[6];
} tcg2_event_t;

static const guid sqtl_guid = { 0xb3e91a47, 0x6c2f, 0x4d01, { 0x8a, 0x71, 0x55, 0x90, 0x2c, 0xde, 0x41, 0xf8 } };
static const guid acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const guid tcg2_guid = { 0x607f766c, 0x7455, 0x42be, { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } };
static const char spec_id[16] = "Spec ID Event03";
static const char squirrel[] = "squirrel";

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

static int guid_eq(const guid *a, const guid *b)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void *find_cfg(system_table *st, const guid *g)
{
    for (u64 i = 0; i < st->ntables; i++)
        if (guid_eq(&st->tables[i].id, g))
            return st->tables[i].table;
    return 0;
}

static int memeq(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static u32 rd32(const void *p)
{
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static u64 rd64(const void *p)
{
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static int check_spec_id(u64 base)
{
    const u8 *p = (const u8 *)(unsigned long)base;
    if (rd32(p + 4) != 0x3)            /* EV_NO_ACTION */
        return 0;
    return memeq(p + 32, spec_id, 16);
}

static int check_sqtl(const sqtl_t *h)
{
    if (!h)
        return 0;
    if (h->Magic != SQTL_MAGIC || h->Version != SQTL_VERSION || h->Format != SQTL_FORMAT)
        return 0;
    if (!(h->Flags & SQTL_F_PRESENT) || !h->LogBase || !h->LogSize || !h->LogLast)
        return 0;
    if (h->LogLast < h->LogBase || h->LogLast >= h->LogBase + h->LogSize)
        return 0;
    if (!h->FinalEvents || !h->FinalEventsSize)
        return 0;
    /* M16: ActiveBanks must advertise SHA-256 when that bank is on. */
    if (!(h->ActiveBanks & TCG2_BOOT_HASH_ALG_SHA256))
        return 0;
    return 1;
}

static const u8 *find_tpm2(system_table *st)
{
    const u8 *rsdp = find_cfg(st, &acpi20);
    if (!rsdp || !memeq(rsdp, "RSD PTR ", 8))
        return 0;
    u64 xsdt = rd64(rsdp + 24);
    if (!xsdt)
        return 0;
    const u8 *x = (const u8 *)(unsigned long)xsdt;
    if (!memeq(x, "XSDT", 4))
        return 0;
    u32 len = rd32(x + 4);
    if (len < 36)
        return 0;
    u32 n = (len - 36) / 8;
    for (u32 i = 0; i < n; i++) {
        u64 addr = rd64(x + 36 + i * 8);
        const u8 *t = (const u8 *)(unsigned long)addr;
        if (memeq(t, "TPM2", 4))
            return t;
    }
    return 0;
}

static u8 map_buf[16384];

EFIAPI status efi_main(handle image, system_table *st)
{
    puts("APP m3-rehearsal");

    const sqtl_t *h = find_cfg(st, &sqtl_guid);
    int cfg_ok = check_sqtl(h);
    puts(cfg_ok ? " cfg=ok" : " cfg=bad");

    int acpi_ok = 0;
    if (cfg_ok) {
        const u8 *tpm2 = find_tpm2(st);
        if (tpm2 && rd32(tpm2 + 4) >= 76) {
            u32 log_size = rd32(tpm2 + 64);
            u64 log_base = rd64(tpm2 + 68);
            acpi_ok = (log_base == h->LogBase && log_size == (u32)h->LogSize);
        }
    }
    puts(acpi_ok ? " acpi=ok" : " acpi=bad");

    int log_ok = cfg_ok && check_spec_id(h->LogBase);
    puts(log_ok ? " log=ok" : " log=bad");

    int tcg2_ok = 0;
    int extend_ok = 0;
    tcg2_proto *tcg2 = 0;
    if (st->bs->locate_protocol((guid *)&tcg2_guid, 0, (void **)&tcg2) == 0 && tcg2) {
        u64 where = 0, last = 0;
        u8 trunc = 0;
        if (tcg2->get_event_log(tcg2, 2, &where, &last, &trunc) == 0 && cfg_ok)
            tcg2_ok = (where == h->LogBase && last == h->LogLast);

        /* HashLogExtendEvent(PCR 8, EV_IPL, "squirrel") — measure line. */
        tcg2_event_t ev;
        ev.size = 24;
        ev.header_size = 14;
        ev.header_version = 1;
        ev.pcr_index = 8;
        ev.event_type = 0x0D;           /* EV_IPL */
        ev.data[0] = 'S';
        ev.data[1] = 'q';
        ev.data[2] = 'T';
        ev.data[3] = 'e';
        ev.data[4] = 's';
        ev.data[5] = 't';
        if (tcg2->hash_log_extend_event(tcg2, 0, (u64)(unsigned long)squirrel, 8, &ev) == 0)
            extend_ok = 1;
    }
    puts(tcg2_ok ? " tcg2=ok" : " tcg2=bad");
    puts(extend_ok ? " extend=ok" : " extend=bad");
    puts("\r\n");

    u64 size, key = 0, dsize = 0;
    u32 ver = 0;
    status s;
    int tries = 0;
    do {
        size = sizeof map_buf;
        s = st->bs->get_memory_map(&size, map_buf, &key, &dsize, &ver);
        if (!s)
            s = st->bs->exit_boot_services(image, key);
    } while (s && ++tries < 4);

    int ebs_ok = 0;
    if (!s && cfg_ok) {
        ebs_ok = check_sqtl(h) && check_spec_id(h->LogBase) &&
                 h->LogLast >= h->LogBase && h->LogLast < h->LogBase + h->LogSize;
    }
    puts(ebs_ok ? "APP m3-rehearsal ebs=ok\r\n" : "APP m3-rehearsal ebs=bad\r\n");

    outb(0xF4, 0);
    for (;;)
        __asm__ volatile("hlt");
}
