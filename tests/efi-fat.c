/* Stand-in EFI application that writes to the FAT volume it was loaded
 * from (roadmap D4: FAT write support in common/efi/fat.c). Original
 * code; built like tests/efi-app.c (static ELF, tests/mkpe.py) for
 * AArch64 and RISC-V. The UEFI types are declared here from the
 * specification, not taken from the firmware. It reports through ConOut:
 *
 *   FAT vol=ok mkdir=ok create=ok write=ok read=ok info=ok append=ok
 *       gap=ok truncate=ok rename=ok list=ok delete=ok grow=ok rmdir=ok
 *       keep=ok
 *
 * all on one line, then "FAT all ok", and returns 0x1234. "keep" leaves
 * "\SquirrelBIOS was here.txt" (a long name) behind with known text, for
 * a check of the volume afterwards (fsck.fat, mtools). */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long uintn;
typedef u64 status;
typedef void *handle;
typedef u16 char16;
typedef status (*fn)();

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } guid;
typedef struct { u64 signature; u32 revision, header_size, crc32, reserved; } table_header;

typedef struct {
    table_header hdr;
    fn raise_tpl, restore_tpl, allocate_pages, free_pages, get_memory_map, allocate_pool,
       free_pool, create_event, set_timer, wait_for_event, signal_event, close_event,
       check_event, install_protocol, reinstall_protocol, uninstall_protocol, handle_protocol;
} boot_services;

typedef struct text_out { fn reset, output_string; } text_out;

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
    void *rt;
    boot_services *bs;
} system_table;

typedef struct {
    u32 revision;
    handle parent;
    system_table *st;
    handle device;
} loaded_image;

typedef struct file {
    u64 revision;
    fn open, close, del, read, write, get_position, set_position, get_info, set_info, flush;
} file;

typedef struct { u64 revision; fn open_volume; } simple_fs;

typedef struct {
    u64 size, file_size, physical_size;
    u8 times[48];
    u64 attribute;
    char16 name[256];
} file_info;

#define READ   1ULL
#define WRITE  2ULL
#define CREATE 0x8000000000000000ULL
#define DIRECTORY 0x10ULL
#define NOT_FOUND (0x8000000000000000ULL | 14)

static const guid loaded_image_guid = { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const guid sfs_guid = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const guid info_guid = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static system_table *st;
static u8 data[6000], back[6000];
static file_info info;

static void puts(const char *s)
{
    char16 buf[128];
    uintn n = 0;
    while (*s && n < 120) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s++;
    }
    buf[n] = 0;
    st->con_out->output_string(st->con_out, buf);
}

static int all = 1;

static void result(const char *name, int ok)
{
    puts(" ");
    puts(name);
    puts(ok ? "=ok" : "=bad");
    all &= ok;
}

static int same(const u8 *a, const u8 *b, uintn n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

static int wsame(const char16 *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (*a != (u8)*b)
            return 0;
    return !*a && !*b;
}

static void wide(char16 *out, const char *s)
{
    while ((*out++ = (u8)*s++))
        ;
}

static u64 size_of(file *f)
{
    uintn n = sizeof info;
    return f->get_info(f, &info_guid, &n, &info) ? ~0ULL : info.file_size;
}

/* Read the whole of name (in d) into back: its size, or ~0 */
static uintn slurp(file *d, const char *name)
{
    char16 w[64];
    wide(w, name);
    file *f;
    if (d->open(d, &f, w, READ, 0ULL))
        return ~0UL;
    uintn n = sizeof back;
    if (f->read(f, &n, back))
        n = ~0UL;
    f->close(f);
    return n;
}

status efi_main(handle image, system_table *systab)
{
    st = systab;
    boot_services *bs = st->bs;
    for (uintn i = 0; i < sizeof data; i++)
        data[i] = (u8)(i * 7 + 3);

    puts("FAT");
    loaded_image *li = 0;
    simple_fs *fs = 0;
    file *root = 0, *dir = 0, *f = 0;
    int ok = !bs->handle_protocol(image, &loaded_image_guid, (void **)&li) && li &&
             !bs->handle_protocol(li->device, &sfs_guid, (void **)&fs) && fs &&
             !fs->open_volume(fs, &root);
    result("vol", ok);
    if (!ok)
        return 0x1234;

    char16 w[64];
    wide(w, "\\SQTEST");
    result("mkdir", !root->open(root, &dir, w, READ | WRITE | CREATE, DIRECTORY));

    /* 3000 bytes: several 512-byte clusters */
    wide(w, "Hello World.txt");
    ok = !dir->open(dir, &f, w, READ | WRITE | CREATE, 0ULL);
    result("create", ok);
    uintn n = 3000;
    ok = ok && !f->write(f, &n, data) && n == 3000;
    if (f)
        f->close(f);
    result("write", ok);
    result("read", slurp(dir, "HELLO WORLD.TXT") == 3000 && same(back, data, 3000));

    ok = !dir->open(dir, &f, w, READ | WRITE, 0ULL);
    result("info", ok && size_of(f) == 3000 && wsame(info.name, "Hello World.txt") &&
                   !(info.attribute & DIRECTORY));
    /* Append 100 bytes at the end, then 10 at 5000: 3100..4999 read as 0 */
    n = 100;
    ok = ok && !f->set_position(f, ~0ULL) && !f->write(f, &n, data + 3000) && size_of(f) == 3100;
    result("append", ok);
    n = 10;
    ok = ok && !f->set_position(f, 5000ULL) && !f->write(f, &n, data + 5000) && size_of(f) == 5010;
    if (ok && slurp(dir, "Hello World.txt") == 5010 && same(back, data, 3100) && same(back + 5000, data + 5000, 10))
        for (uintn i = 3100; i < 5000; i++)
            ok &= back[i] == 0;
    else
        ok = 0;
    result("gap", ok);

    /* SetInfo: a smaller size, then a new name */
    ok = size_of(f) == 5010;
    info.file_size = 1000;
    ok = ok && !f->set_info(f, &info_guid, sizeof info, &info) && size_of(f) == 1000;
    result("truncate", ok && slurp(dir, "Hello World.txt") == 1000 && same(back, data, 1000));
    ok = size_of(f) == 1000;
    wide(info.name, "RENAMED.BIN");
    ok = ok && !f->set_info(f, &info_guid, sizeof info, &info);
    file *g = 0;
    wide(w, "Hello World.txt");
    ok = ok && dir->open(dir, &g, w, READ, 0ULL) == NOT_FOUND;
    result("rename", ok && slurp(dir, "renamed.bin") == 1000 && same(back, data, 1000));

    /* The directory lists ".", ".." and RENAMED.BIN */
    int dots = 0, renamed = 0, others = 0;
    dir->set_position(dir, 0ULL);
    for (;;) {
        n = sizeof info;
        if (dir->read(dir, &n, &info) || !n)
            break;
        if (wsame(info.name, ".") || wsame(info.name, ".."))
            dots++;
        else if (wsame(info.name, "RENAMED.BIN") && info.file_size == 1000)
            renamed++;
        else
            others++;
    }
    result("list", dots == 2 && renamed == 1 && !others);

    ok = !f->del(f);
    result("delete", ok && slurp(dir, "RENAMED.BIN") == ~0UL);

    /* 40 long names: the directory grows past its first cluster */
    char name[] = "a long file name 00.data";
    ok = 1;
    for (int i = 0; i < 40 && ok; i++) {
        name[17] = (char)('0' + i / 10);
        name[18] = (char)('0' + i % 10);
        wide(w, name);
        n = 20;
        ok = !dir->open(dir, &g, w, READ | WRITE | CREATE, 0ULL) && !g->write(g, &n, data + i) &&
             !g->close(g);
    }
    int count = 0;
    dir->set_position(dir, 0ULL);
    for (;;) {
        n = sizeof info;
        if (dir->read(dir, &n, &info) || !n)
            break;
        count++;
    }
    name[17] = '3';
    name[18] = '7';
    ok = ok && count == 42 && slurp(dir, name) == 20 && same(back, data + 37, 20);
    result("grow", ok);

    /* A directory that is not empty stays; emptied, it goes */
    file *again = 0;
    wide(w, "\\SQTEST");
    int kept = !root->open(root, &again, w, READ | WRITE, 0ULL) && again->del(again) == 2;
    for (int i = 0; i < 40 && ok; i++) {
        name[17] = (char)('0' + i / 10);
        name[18] = (char)('0' + i % 10);
        wide(w, name);
        ok = !dir->open(dir, &g, w, READ | WRITE, 0ULL) && !g->del(g);
    }
    ok = ok && kept && !dir->del(dir);
    wide(w, "\\SQTEST");
    result("rmdir", ok && root->open(root, &again, w, READ, 0ULL) == NOT_FOUND);

    static const char text[] = "written by SquirrelBIOS\n";
    wide(w, "\\SquirrelBIOS was here.txt");
    n = sizeof text - 1;
    ok = !root->open(root, &f, w, READ | WRITE | CREATE, 0ULL) && !f->write(f, &n, (void *)text) &&
         !f->close(f);
    result("keep", ok && slurp(root, "SQUIRREL BIOS WAS HERE.TXT") == ~0UL &&
                     slurp(root, "squirrelbios WAS HERE.txt") == sizeof text - 1);
    root->close(root);
    puts(all ? "\nFAT all ok\n" : "\nFAT failed\n");
    return 0x1234;
}
