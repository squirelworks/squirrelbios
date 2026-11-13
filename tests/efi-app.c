/* Stand-in EFI application for the device-tree UEFI firmware
 * (aarch64-uefi, riscv64-uefi; roadmap D1/D3). Original code.
 *
 * Built as a static ELF linked at 256 MB + 4 KB and turned into a PE32+
 * image by tests/mkpe.py. Its preferred base is not RAM on QEMU virt, so
 * the firmware must relocate it. The UEFI tables are declared here from
 * the specification, not from the firmware's headers, so a layout
 * mistake on either side shows. It reports through ConOut:
 *
 *   APP <arch> st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok
 *   APP con=ok timer=ok time=ok var=ok dtb=ok
 *
 * Started as a -kernel image, it also prints its LoadOptions (the
 * -append line) and the size of the -initrd it gets the way a Linux EFI
 * stub does (LocateDevicePath on the initrd media path, then LoadFile2):
 *
 *   APP opts="..." initrd=<bytes>
 *
 * then returns 0x1234; with -DEXIT it calls Exit(0x5678) instead; with
 * -DEBS it calls ExitBootServices and then ResetSystem(shutdown) through
 * the runtime services (QEMU exits 0), or a cold reset when a check after
 * ExitBootServices fails (QEMU keeps running and the test times out). */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long uintn;
typedef u64 status;
typedef void *handle;
typedef u16 char16;

#if defined(__aarch64__)
#define ARCH "aarch64"
#elif defined(__riscv)
#define ARCH "riscv64"
#endif

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } guid;

typedef struct {
    u64 signature;
    u32 revision, header_size, crc32, reserved;
} table_header;

typedef status (*fn)();

typedef struct {
    table_header hdr;
    fn raise_tpl, restore_tpl, allocate_pages, free_pages, get_memory_map, allocate_pool,
       free_pool, create_event, set_timer, wait_for_event, signal_event, close_event,
       check_event, install_protocol, reinstall_protocol, uninstall_protocol, handle_protocol,
       reserved, register_protocol_notify, locate_handle, locate_device_path,
       install_configuration_table, load_image, start_image, exit, unload_image,
       exit_boot_services, get_next_monotonic_count, stall, set_watchdog_timer,
       connect_controller, disconnect_controller, open_protocol, close_protocol,
       open_protocol_information, protocols_per_handle, locate_handle_buffer, locate_protocol,
       install_multiple, uninstall_multiple, calculate_crc32, copy_mem, set_mem, create_event_ex;
} boot_services;

typedef struct {
    table_header hdr;
    fn get_time, set_time, get_wakeup_time, set_wakeup_time, set_virtual_address_map,
       convert_pointer, get_variable, get_next_variable_name, set_variable,
       get_next_high_monotonic_count, reset_system, update_capsule, query_capsule_capabilities,
       query_variable_info;
} runtime_services;

typedef struct text_out {
    fn reset, output_string, test_string, query_mode, set_mode, set_attribute, clear_screen,
       set_cursor_position, enable_cursor;
    void *mode;
} text_out;

typedef struct { guid vendor; void *table; } config_table;

typedef struct {
    table_header hdr;
    char16 *firmware_vendor;
    u32 firmware_revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    text_out *con_out;
    handle stderr_handle;
    void *std_err;
    runtime_services *rt;
    boot_services *bs;
    uintn n_tables;
    config_table *tables;
} system_table;

typedef struct {
    u32 revision;
    handle parent;
    system_table *st;
    handle device;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 code_type, data_type;
    fn unload;
} loaded_image;

typedef struct {
    u16 year;
    u8 month, day, hour, minute, second, pad1;
    u32 nanosecond;
    short timezone;
    u8 daylight, pad2;
} efi_time;

#define ERR(n) (0x8000000000000000ULL | (n))
#define BUFFER_TOO_SMALL ERR(5)
#define NOT_READY ERR(6)
#define NOT_FOUND ERR(14)

static const guid loaded_image_guid = { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const guid dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
static const guid load_file2_guid = { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };
static const guid var_guid = { 0x53717153, 0x6170, 0x7041, { 0x70, 0x54, 0x65, 0x73, 0x74, 0x56, 0x61, 0x72 } };

static system_table *st;
static boot_services *bs;
static u8 map_buf[16384];

/* An absolute pointer the firmware has to relocate */
static const char app_name[] = "APP";
static const char *volatile abs_name = app_name;

static void puts(const char *s)
{
    char16 buf[128];
    uintn n = 0;
    while (*s) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s++;
        if (n >= 120) {
            buf[n] = 0;
            st->con_out->output_string(st->con_out, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    st->con_out->output_string(st->con_out, buf);
}

static void result(const char *name, int ok)
{
    puts(" ");
    puts(name);
    puts(ok ? "=ok" : "=bad");
}

static int guid_eq(const guid *a, const guid *b)
{
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;
    for (int i = 0; i < 16; i++)
        if (p[i] != q[i])
            return 0;
    return 1;
}

/* The Linux initrd media device path: vendor media node, then the end */
static const struct {
    u8 type, subtype, len[2];
    guid vendor;
    u8 end[4];
} __attribute__((packed)) initrd_path = {
    4, 3, { 20, 0 },
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } },
    { 0x7F, 0xFF, 4, 0 },
};

typedef struct load_file2 {
    status (*load_file)(struct load_file2 *, void *, u8, uintn *, void *);
} load_file2;

static void put_dec(u64 v)
{
    char b[24];
    int n = 23;
    b[n] = 0;
    do {
        b[--n] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts(b + n);
}

static void options_and_initrd(loaded_image *li)
{
    if (!li || !li->load_options_size)
        return;
    char buf[100];
    const char16 *o = li->load_options;
    uintn n = 0;
    while (n < sizeof buf - 1 && n < li->load_options_size / 2 && o[n])
        buf[n] = (char)o[n], n++;
    buf[n] = 0;
    puts("APP opts=\"");
    puts(buf);
    puts("\" initrd=");
    void *path = (void *)&initrd_path;
    handle h = 0;
    load_file2 *lf = 0;
    uintn size = 0;
    if (bs->locate_device_path(&load_file2_guid, &path, &h) == 0 &&
        bs->handle_protocol(h, &load_file2_guid, (void **)&lf) == 0 &&
        lf->load_file(lf, path, 0, &size, 0) == BUFFER_TOO_SMALL && size) {
        void *p = 0;
        uintn got = size;
        if (bs->allocate_pool(2, size, &p) == 0 && lf->load_file(lf, path, 0, &got, p) == 0 && got == size) {
            put_dec(size);
            puts(((u8 *)p)[0] == 'S' && ((u8 *)p)[1] == 'q' ? " data=ok" : " data=bad");
        } else
            puts("load=bad");
        if (p)
            bs->free_pool(p);
    } else
        puts("none");
    puts("\n");
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

status efi_main(handle image, system_table *systab)
{
    st = systab;
    bs = st->bs;
    puts("APP " ARCH);
    result("st", st->hdr.signature == 0x5453595320494249ULL);
    result("bs", bs->hdr.signature == 0x56524553544f4f42ULL);

    loaded_image *li = 0;
    int ok = bs->handle_protocol(image, &loaded_image_guid, (void **)&li) == 0 && li &&
             li->st == st && li->parent && li->image_size >= 0x2000;
    result("li", ok);
    extern char __image_base[];
    result("base", ok && li->image_base == (void *)__image_base);
    result("reloc", abs_name == app_name);

    void *p = 0;
    ok = bs->allocate_pool(2 /* EfiLoaderData */, 200, &p) == 0 && p;
    if (ok) {
        ((u8 *)p)[199] = 0x5A;
        ok = bs->free_pool(p) == 0;
    }
    result("pool", ok);

    uintn size = 0, key = 0, dsize = 0;
    u32 dver = 0;
    ok = bs->get_memory_map(&size, 0, &key, &dsize, &dver) == BUFFER_TOO_SMALL && size;
    size = sizeof map_buf;
    ok = ok && bs->get_memory_map(&size, map_buf, &key, &dsize, &dver) == 0 && dsize >= 40 && dver == 1;
    result("map", ok);
    puts("\nAPP");

    /* ConOut: a mode query and an attribute */
    uintn cols = 0, rows = 0;
    ok = st->con_out->query_mode(st->con_out, 0, &cols, &rows) == 0 && cols == 80 && rows == 25;
    result("con", ok);

    /* A one-shot timer of 10 ms, waited for */
    void *ev = 0;
    uintn idx = 99;
    ok = bs->create_event(0x80000000 /* EVT_TIMER */, 8, 0, 0, &ev) == 0 &&
         bs->set_timer(ev, 2 /* TimerRelative */, 100000) == 0 &&
         bs->check_event(ev) == NOT_READY &&
         bs->wait_for_event(1, &ev, &idx) == 0 && idx == 0;
    if (ev)
        bs->close_event(ev);
    result("timer", ok);

    /* GetTime from the RTC: a plausible date */
    efi_time t;
    ok = st->rt->get_time(&t, 0) == 0 && t.year >= 2020 && t.year < 2200 &&
         t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour < 24;
    result("time", ok);

    /* A variable set, read back and deleted */
    static char16 name[] = u"SqTest";
    u32 value = 0x5A17, back = 0, attr = 0;
    uintn vsize = sizeof back;
    ok = st->rt->set_variable(name, &var_guid, 7, sizeof value, &value) == 0 &&
         st->rt->get_variable(name, &var_guid, &attr, &vsize, &back) == 0 &&
         back == value && vsize == 4 && attr == 7 &&
         st->rt->set_variable(name, &var_guid, 7, 0, 0) == 0 &&
         st->rt->get_variable(name, &var_guid, &attr, &vsize, &back) == NOT_FOUND;
    result("var", ok);

    /* The device tree among the configuration tables */
    ok = 0;
    for (uintn i = 0; i < st->n_tables; i++)
        if (guid_eq(&st->tables[i].vendor, &dtb_guid))
            ok = be32(st->tables[i].table) == 0xd00dfeed;
    result("dtb", ok);
    puts("\n");
    options_and_initrd(li);

#if defined(EXIT)
    bs->exit(image, 0x5678, 0, 0);
    puts("exit=returned\n");
#elif defined(EBS)
    size = sizeof map_buf;
    bs->get_memory_map(&size, map_buf, &key, &dsize, &dver);
    runtime_services *rt = st->rt;
    status s = bs->exit_boot_services(image, key);
    ok = s == 0 && st->bs == 0;
    rt->reset_system(ok ? 2 /* shutdown */ : 0 /* cold */, 0, 0, 0);
    for (;;)
        ;
#endif
    return 0x1234;
}
