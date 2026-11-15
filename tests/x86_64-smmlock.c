/* Stand-in for a third-party EFI image that tries to take over SMM on
 * q35 (roadmap F2). Original code, written from the Intel 3 Series MCH
 * datasheet (SMRAM 9Dh, ESMRAMC 9Eh). Built like tests/x86_64-einj.c. By
 * the time any image runs, the firmware must have locked SMRAM (D_LCK),
 * so the image can neither open the legacy SMRAM window nor turn TSEG
 * off, and TSEG reads as nothing. Reports on COM1:
 *
 *   APP smmlock dlck=ok open=refused tseg=closed
 *
 * dlck    the SMRAM register has D_LCK set
 * open    writing D_OPEN and clearing T_EN changes neither register
 * tseg    the 1 MB reserved range under the top of low RAM reads as all
 *         ones (QEMU's TSEG blackhole), not the SMI handler
 *
 * or "APP smmlock none" without a q35 MCH; then returns 0. */

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

static u8 cfg_read8(u8 off)
{
    outl(0xCF8, 0x80000000u | (off & 0xFCu));
    return inb((u16)(0xCFC + (off & 3)));
}

static void cfg_write8(u8 off, u8 v)
{
    outl(0xCF8, 0x80000000u | (off & 0xFCu));
    outb((u16)(0xCFC + (off & 3)), v);
}

static u16 cfg_read16(u8 off)
{
    outl(0xCF8, 0x80000000u | (off & 0xFCu));
    return inw((u16)(0xCFC + (off & 2)));
}

static u8 map[16384];

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    puts("APP smmlock");
    if (cfg_read16(0) != 0x8086 || cfg_read16(2) != 0x29C0) {
        puts(" none\r\n");
        return 0;
    }
    u8 smram = cfg_read8(0x9D), esmramc = cfg_read8(0x9E);
    puts(smram & 0x10 ? " dlck=ok" : " dlck=bad");

    cfg_write8(0x9D, (u8)(smram | 0x40));           /* D_OPEN */
    cfg_write8(0x9E, (u8)(esmramc & ~1u));          /* T_EN off */
    u8 s2 = cfg_read8(0x9D), e2 = cfg_read8(0x9E);
    int refused = !(s2 & 0x40) && (e2 & 1) == (esmramc & 1) && (esmramc & 1);
    puts(refused ? " open=refused" : " open=bad");
    if (!refused) {                                 /* leave it as it was */
        cfg_write8(0x9E, esmramc);
        cfg_write8(0x9D, smram);
    }

    u64 size = sizeof map, key = 0, dsize = 0;
    u32 ver = 0;
    const char *t = " tseg=bad";
    if (!st->bs->get_memory_map(&size, map, &key, &dsize, &ver) && dsize) {
        for (u64 off = 0; off + dsize <= size; off += dsize) {
            const u8 *d = map + off;
            u32 type = *(const u32 *)d;
            u64 start = *(const u64 *)(d + 8), pages = *(const u64 *)(d + 24);
            if (type == 0 && pages == 256 && start >= 0x4000000 && start < 0x100000000ull) {
                volatile const u32 *p = (volatile const u32 *)start;
                int ones = 1;
                for (int i = 0; i < 0x2000; i += 0x100)  /* GDT, handler, stacks */
                    if (p[i] != 0xFFFFFFFFu)
                        ones = 0;
                t = ones ? " tseg=closed" : " tseg=open";
            }
        }
    }
    puts(t);
    puts("\r\n");
    return 0;
}
