/* Measured TPM event-log hand-off + measured-boot polish stand-in
 * (Phase G M5 / G-measured-handoff, M16 / G-tpm-polish).
 * Original code, written from the TCG EFI Protocol Spec (GetEventLog,
 * EFI_TCG2_FINAL_EVENTS_TABLE), the TCG ACPI Spec (TPM2 log area), the
 * TCG PC Client Platform Firmware Profile (TCG2 crypto-agile log and
 * PCR usage), and the Squirrel SQTL contract in common/handoff.md.
 *
 * With swtpm + tpm-tis, reports on COM1:
 *
 *   APP tpm-log cfg=ok acpi=ok log=ok tcg2=ok banks=ok parse=ok policy=ok
 *   … ExitBootServices …
 *   APP tpm-log ebs=ok
 *
 * cfg    SQTL configuration table: magic 'SQTL', version 1, format TCG2,
 *        PRESENT, LogBase/LogLast non-zero and LogLast in [Base, Base+Size)
 * acpi   ACPI 2.0 TPM2 table log area matches SQTL LogBase / LogSize
 * log    bytes at LogBase start with Spec ID Event03 (TCG2 header event)
 * tcg2   EFI_TCG2_PROTOCOL GetEventLog returns the same base/last as SQTL
 * banks  SQTL ActiveBanks has EFI_TCG2_BOOT_HASH_ALG_SHA256 (0x2)
 * parse  walk the TCG2 log with only public field sizes (no firmware headers)
 * policy firmware PCR map matches common/handoff.md (policy v1)
 * ebs    after ExitBootServices, SQTL still readable and Spec ID intact
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
#define EFI_ALG_SHA256  0x00000002u

#define EV_NO_ACTION                    0x3u
#define EV_SEPARATOR                    0x4u
#define EV_S_CRTM_VERSION               0x8u
#define EV_EFI_VARIABLE_DRIVER_CONFIG   0x80000001u
#define EV_EFI_BOOT_SERVICES_APPLICATION 0x80000003u
#define EV_EFI_ACTION                   0x80000007u
#define EV_EFI_PLATFORM_FIRMWARE_BLOB   0x80000008u

typedef struct {
    u32 Magic, Version, Format, Flags;
    u64 LogBase, LogSize, LogLast, FinalEvents;
    u32 FinalEventsSize, ActiveBanks;
} sqtl_t;

typedef status (EFIAPI *tcg2_get_event_log_fn)(void *this, u32 format, u64 *where,
                                               u64 *last, u8 *truncated);

typedef struct {
    void *get_capability;
    tcg2_get_event_log_fn get_event_log;
} tcg2_proto;

static const guid sqtl_guid = { 0xb3e91a47, 0x6c2f, 0x4d01, { 0x8a, 0x71, 0x55, 0x90, 0x2c, 0xde, 0x41, 0xf8 } };
static const guid acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const guid tcg2_guid = { 0x607f766c, 0x7455, 0x42be, { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } };
static const char spec_id[16] = "Spec ID Event03";

/* Digest size by TPM algorithm id (TCG Algorithm Registry). */
static u32 alg_size(u16 alg)
{
    if (alg == 0x0004) return 20;
    if (alg == 0x000B) return 32;
    if (alg == 0x000C) return 48;
    if (alg == 0x000D) return 64;
    return 0;
}

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

static u16 rd16(const void *p)
{
    u16 v;
    __builtin_memcpy(&v, p, 2);
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
    if (rd32(p + 4) != EV_NO_ACTION)
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
    return 1;
}

/* Walk XSDT for signature "TPM2"; return table pointer or 0. */
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

/* Skip Spec ID Event03 (TCG_PCClientPCREvent SHA-1 header + event).
 * Returns offset of the first TCG_PCR_EVENT2, or 0 on failure. */
static u32 skip_spec_id(const u8 *log, u32 cap, u16 *algs, u32 *nalgs)
{
    if (cap < 36 || rd32(log + 4) != EV_NO_ACTION || !memeq(log + 32, spec_id, 16))
        return 0;
    u32 esize = rd32(log + 28);
    if (esize < 28 || 32 + esize > cap)
        return 0;
    const u8 *e = log + 32;
    u32 n = rd32(e + 24);
    if (n == 0 || n > 8 || 28 + 4 * n + 1 > esize)
        return 0;
    for (u32 i = 0; i < n; i++) {
        u16 alg = rd16(e + 28 + 4 * i);
        u16 sz = rd16(e + 30 + 4 * i);
        if (!alg_size(alg) || alg_size(alg) != sz)
            return 0;
        algs[i] = alg;
    }
    *nalgs = n;
    return 32 + esize;
}

/* Advance one TCG_PCR_EVENT2; returns bytes consumed, or 0 on failure. */
static u32 next_event2(const u8 *p, u32 left, u32 nalgs, const u16 *algs,
                       u32 *pcr, u32 *etype)
{
    if (left < 12)
        return 0;
    *pcr = rd32(p);
    *etype = rd32(p + 4);
    u32 count = rd32(p + 8);
    if (*pcr == 0 && *etype == 0 && count == 0)
        return 0;
    if (count != nalgs)
        return 0;
    u32 at = 12;
    for (u32 i = 0; i < count; i++) {
        if (at + 2 > left)
            return 0;
        u16 alg = rd16(p + at);
        u32 sz = alg_size(alg);
        if (!sz || alg != algs[i] || at + 2 + sz > left)
            return 0;
        at += 2 + sz;
    }
    if (at + 4 > left)
        return 0;
    u32 esize = rd32(p + at);
    at += 4;
    if (at + esize > left)
        return 0;
    return at + esize;
}

/* Policy v1 (common/handoff.md): expected (PCR, EventType) prefix after Spec ID. */
static const u32 expect[][2] = {
    { 0, EV_S_CRTM_VERSION },
    { 0, EV_EFI_PLATFORM_FIRMWARE_BLOB },
    { 7, EV_EFI_VARIABLE_DRIVER_CONFIG },
    { 7, EV_EFI_VARIABLE_DRIVER_CONFIG },
    { 7, EV_EFI_VARIABLE_DRIVER_CONFIG },
    { 7, EV_EFI_VARIABLE_DRIVER_CONFIG },
    { 7, EV_EFI_VARIABLE_DRIVER_CONFIG },
    { 4, EV_EFI_ACTION },
    { 0, EV_SEPARATOR },
    { 1, EV_SEPARATOR },
    { 2, EV_SEPARATOR },
    { 3, EV_SEPARATOR },
    { 4, EV_SEPARATOR },
    { 5, EV_SEPARATOR },
    { 6, EV_SEPARATOR },
    { 7, EV_SEPARATOR },
    { 4, EV_EFI_BOOT_SERVICES_APPLICATION },
};
#define NEXPECT (sizeof expect / sizeof expect[0])

static int walk_log(u64 base, u64 last, u64 log_size, int *parse_ok, int *policy_ok)
{
    const u8 *log = (const u8 *)(unsigned long)base;
    if (last < base || !log_size || log_size > 0x10000)
        return 0;
    u32 cap = (u32)log_size;
    u64 last_off = last - base;
    if (last_off >= cap)
        return 0;

    u16 algs[8];
    u32 nalgs = 0;
    u32 at = skip_spec_id(log, cap, algs, &nalgs);
    if (!at)
        return 0;

    u32 got = 0;
    int parse = 1;
    int policy = 1;
    int saw_last = 0;

    while (at < cap) {
        u32 pcr = 0, etype = 0;
        u32 used = next_event2(log + at, cap - at, nalgs, algs, &pcr, &etype);
        if (!used) {
            parse = 0;
            break;
        }
        if (at == (u32)last_off)
            saw_last = 1;
        if (got < NEXPECT) {
            if (pcr != expect[got][0] || etype != expect[got][1])
                policy = 0;
            got++;
        }
        at += used;
        if (saw_last)
            break;
        if (at > (u32)last_off) {
            parse = 0;
            break;
        }
    }
    if (!saw_last)
        parse = 0;
    if (got < NEXPECT)
        policy = 0;
    *parse_ok = parse;
    *policy_ok = policy;
    return 1;
}

static u8 map_buf[16384];

EFIAPI status efi_main(handle image, system_table *st)
{
    puts("APP tpm-log");

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
    tcg2_proto *tcg2 = 0;
    if (st->bs->locate_protocol((guid *)&tcg2_guid, 0, (void **)&tcg2) == 0 && tcg2) {
        u64 where = 0, last = 0;
        u8 trunc = 0;
        if (tcg2->get_event_log(tcg2, 2, &where, &last, &trunc) == 0 && cfg_ok)
            tcg2_ok = (where == h->LogBase && last == h->LogLast);
    }
    puts(tcg2_ok ? " tcg2=ok" : " tcg2=bad");

    int banks_ok = cfg_ok && (h->ActiveBanks & EFI_ALG_SHA256);
    puts(banks_ok ? " banks=ok" : " banks=bad");

    int parse_ok = 0, policy_ok = 0;
    if (cfg_ok && log_ok)
        walk_log(h->LogBase, h->LogLast, h->LogSize, &parse_ok, &policy_ok);
    puts(parse_ok ? " parse=ok" : " parse=bad");
    puts(policy_ok ? " policy=ok" : " policy=bad");
    puts("\r\n");

    /* ExitBootServices: SQTL and the ACPI-NVS log must remain usable. */
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
        /* Re-read through the same pointer; LogLast may have advanced. */
        ebs_ok = check_sqtl(h) && check_spec_id(h->LogBase) &&
                 h->LogLast >= h->LogBase && h->LogLast < h->LogBase + h->LogSize &&
                 (h->ActiveBanks & EFI_ALG_SHA256);
    }
    puts(ebs_ok ? "APP tpm-log ebs=ok\r\n" : "APP tpm-log ebs=bad\r\n");

    outb(0xF4, 0);
    for (;;)
        __asm__ volatile("hlt");
}
