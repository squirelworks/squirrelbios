/* Stand-in OS for EINJ error injection on x86-64 (roadmap F2).
 * Original code, written from the ACPI 6.5 specification (18.6 EINJ,
 * 18.5 APEI instructions, 18.3.2.7 GHES) and the UEFI CPER memory error
 * section. Built as a static ELF turned into a PE by tests/mkpe.py. It
 * finds EINJ and the HEST's generic hardware error source through the
 * ACPI tables and runs the EINJ actions as an OS's APEI code would (read
 * and write register instructions on memory and I/O), then reads the
 * error status block. Reports on COM1:
 *
 *   APP einj types=0x8 inject=ok ghes=ok ap=ok refused=ok
 *
 * types    GET_ERROR_TYPE: correctable memory errors only
 * inject   SET_ERROR_TYPE 0x8, EXECUTE_OPERATION (an SMI), not busy, status 0,
 *          the trigger action table run
 * ghes     the error status block holds one corrected memory error (CPER)
 * ap       the same from an application processor (MP services), whose
 *          SMI must land in SMRAM too; "none" with one CPU
 * refused  an uncorrectable memory error (0x10) gets a failed status
 *
 * then returns 0. */

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
typedef struct {
    fn get_number_of_processors, get_processor_info, startup_all_aps, startup_this_ap;
} mp_services;

static const guid acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const guid mp_guid = { 0x3fdda605, 0xa76e, 0x4f46, { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 } };
static const u8 mem_err[16] = {
    0x14, 0x11, 0xBC, 0xA5, 0x64, 0x6F, 0xDE, 0x4E, 0xB8, 0x63, 0x3E, 0x83, 0xED, 0x7C, 0x83, 0xB1,
};

static void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

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

static void puthex(u64 v)
{
    puts("0x");
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)(v >> i) & 15;
        if (d || started || i == 0) {
            putc("0123456789abcdef"[d]);
            started = 1;
        }
    }
}

static int same(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

static const u8 *find_table(system_table *st, const char *sig)
{
    for (u64 i = 0; i < st->ntables; i++) {
        if (!same(&st->tables[i].id, &acpi20, 16))
            continue;
        const u8 *rsdp = st->tables[i].table;
        const u8 *xsdt = (const u8 *)rd64(rsdp + 24);
        u32 n = (rd32(xsdt + 4) - 36) / 8;
        for (u32 j = 0; j < n; j++) {
            const u8 *t = (const u8 *)rd64(xsdt + 36 + 8 * j);
            if (same(t, sig, 4))
                return t;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- APEI */

static u64 reg_read(const u8 *gas)
{
    u64 a = rd64(gas + 4);
    u8 w = gas[1];
    if (gas[0] == 1)
        return w == 8 ? inb((u16)a) : w == 16 ? inw((u16)a) : inl((u16)a);
    volatile u8 *p = (volatile u8 *)a;
    return w == 8 ? *p : w == 16 ? *(volatile u16 *)p : w == 32 ? *(volatile u32 *)p : *(volatile u64 *)p;
}

static void reg_write(const u8 *gas, u64 v)
{
    u64 a = rd64(gas + 4);
    u8 w = gas[1];
    if (gas[0] == 1) {
        if (w == 8)
            outb((u16)a, (u8)v);
        else if (w == 16)
            outw((u16)a, (u16)v);
        else
            outl((u16)a, (u32)v);
        return;
    }
    volatile u8 *p = (volatile u8 *)a;
    if (w == 8)
        *p = (u8)v;
    else if (w == 16)
        *(volatile u16 *)p = (u16)v;
    else if (w == 32)
        *(volatile u32 *)p = (u32)v;
    else
        *(volatile u64 *)p = v;
}

/* Run every entry for action; value in and out, as APEI's interpreter */
static u64 run(const u8 *entries, u32 n, u8 action, u64 value)
{
    for (u32 i = 0; i < n; i++) {
        const u8 *e = entries + 32 * i;
        if (e[0] != action)
            continue;
        u64 ev = rd64(e + 16), mask = rd64(e + 24);
        switch (e[1]) {
        case 0: value = reg_read(e + 4) & mask; break;
        case 1: value = (reg_read(e + 4) & mask) == ev; break;
        case 2: reg_write(e + 4, value & mask); break;
        case 3: reg_write(e + 4, ev & mask); break;
        default: break;
        }
    }
    return value;
}

static const u8 *einj_entries;
static u32 einj_n;
static volatile u64 inject_status;

/* The OS's side of one injection: 0, or what went wrong */
static u64 inject(u64 type)
{
    run(einj_entries, einj_n, 0, 0);                    /* BEGIN */
    run(einj_entries, einj_n, 2, type);                 /* SET_ERROR_TYPE */
    run(einj_entries, einj_n, 5, 0);                    /* EXECUTE */
    for (int i = 0; i < 1000 && (run(einj_entries, einj_n, 6, 0) & 1); i++)
        ;                                               /* CHECK_BUSY */
    u64 st = run(einj_entries, einj_n, 7, 0);           /* GET_COMMAND_STATUS */
    const u8 *tt = (const u8 *)run(einj_entries, einj_n, 1, 0);
    if (tt && rd32(tt) == 16)
        run(tt + 16, rd32(tt + 12), 0xFF, 0);           /* the trigger actions */
    run(einj_entries, einj_n, 4, 0);                    /* END */
    return st;
}

static void EFIAPI inject_on_ap(void *arg)
{
    (void)arg;
    inject_status = inject(0x8);
}

/* The GHES block: one corrected memory error, which this reader clears */
static int read_ghes(const u8 *ghes_entry)
{
    u8 *block = (u8 *)rd64((const u8 *)rd64(ghes_entry + 24));
    u32 bs = rd32(block);
    int ok = (bs & 2) && (bs >> 4 & 0x3FF) == 1 && rd32(block + 16) == 2 &&
             same(block + 20, mem_err, 16) && rd32(block + 20 + 24) == 80 &&
             block[20 + 72 + 72] == 2;
    *(volatile u32 *)block = 0;
    return ok;
}

static const u8 *find_ghes(const u8 *hest)
{
    const u8 *p = hest + 40, *end = hest + rd32(hest + 4);
    for (u32 i = 0; i < rd32(hest + 36) && p < end; i++) {
        u16 type = (u16)(p[0] | p[1] << 8);
        if (type == 9)
            return p;
        if (type == 0)
            p += 40 + 28 * p[32];
        else if (type == 1)
            p += 48 + 28 * p[44];
        else
            return 0;
    }
    return 0;
}

static void result(const char *label, int ok)
{
    puts(label);
    puts(ok ? "ok" : "bad");
}

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    puts("APP einj");
    const u8 *einj = find_table(st, "EINJ"), *hest = find_table(st, "HEST");
    const u8 *ghes = hest ? find_ghes(hest) : 0;
    if (!einj || !ghes) {
        puts(einj ? " no GHES =bad\r\n" : " no EINJ =bad\r\n");
        return 1;
    }
    einj_entries = einj + 48;
    einj_n = rd32(einj + 44);
    u64 types = run(einj_entries, einj_n, 3, 0);
    puts(" types=");
    puthex(types);
    result(" inject=", inject(0x8) == 0);
    result(" ghes=", read_ghes(ghes));

    mp_services *mp = 0;
    u64 cpus = 1, enabled = 1;
    if (!st->bs->locate_protocol((void *)&mp_guid, (void *)0, (void **)&mp) && mp)
        mp->get_number_of_processors(mp, &cpus, &enabled);
    if (cpus > 1) {
        inject_status = ~0ull;
        status s = mp->startup_this_ap(mp, (void *)inject_on_ap, (u64)(cpus - 1), (void *)0, 0ull,
                                       (void *)0, (void *)0);
        result(" ap=", !s && inject_status == 0 && read_ghes(ghes));
    } else {
        puts(" ap=none");
    }
    result(" refused=", inject(0x10) != 0);
    puts("\r\n");
    return 0;
}
