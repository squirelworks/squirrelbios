/* Project-key capsule + ESRT fwupd-shaped dump (Phase G M4). Original.
 *
 * Trust: fw_cfg opt/org.squirrelworks/capsule-db = capsule-project.esl
 * Capsule: capsule_project_blob (signed by seed 0x5153_C001).
 *
 * Prints (COM1), before and after UpdateCapsule:
 *   APP esrt-list count=N version=V max=M
 *   APP esrt-entry0 class=<uuid> type=T fw=0x... lowest=0x... flags=0x...
 *                   last_ver=0x... last_status=N
 * then:
 *   APP capsule-key list=ok update=ok esrt=ok
 *
 * Host tool tools/esrt_standin.py checks the same shape fwupd-class
 * tooling expects under QEMU. */
#include "capsule_project_blob.h"

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
typedef struct {
    table_header hdr;
    fn get_time, set_time, get_wakeup_time, set_wakeup_time, set_virtual_address_map,
       convert_pointer, get_variable, get_next_variable_name, set_variable,
       get_next_high_monotonic_count, reset_system, update_capsule, query_capsule_capabilities,
       query_variable_info;
} runtime_services;
typedef struct { guid id; void *table; } config_table;
typedef struct {
    table_header hdr; void *vendor; u32 revision;
    handle con_in_handle; void *con_in; handle con_out_handle; void *con_out;
    handle stderr_handle; void *std_err; runtime_services *rt; boot_services *bs;
    u64 ntables; config_table *tables;
} system_table;
typedef struct { guid capsule_guid; u32 header_size, flags, capsule_image_size; } capsule_header;
typedef struct {
    guid fw_class; u32 fw_type, fw_version, lowest_supported_fw_version, capsule_flags;
    u32 last_attempt_version, last_attempt_status;
} esrt_entry;
typedef struct {
    u32 fw_resource_count, fw_resource_count_max; u64 fw_resource_version; esrt_entry entries[];
} esrt_table;

static void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void putc(char c) { while (!(inb(0x3FD) & 0x20)); outb(0x3F8, (u8)c); }
static void puts(const char *s) { while (*s) putc(*s++); }

static void put_hex_nibble(u8 v)
{
    v &= 0xf;
    putc((char)(v < 10 ? '0' + v : 'a' + (v - 10)));
}

static void put_hex8(u8 v)
{
    put_hex_nibble((u8)(v >> 4));
    put_hex_nibble(v);
}

static void put_hex16(u16 v)
{
    put_hex8((u8)(v >> 8));
    put_hex8((u8)v);
}

static void put_hex32(u32 v)
{
    put_hex8((u8)(v >> 24));
    put_hex8((u8)(v >> 16));
    put_hex8((u8)(v >> 8));
    put_hex8((u8)v);
}

static void put_u32(u32 v)
{
    char buf[11];
    int i = 0;
    if (!v) { putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) putc(buf[i]);
}

static void put_guid(const guid *g)
{
    /* RFC-4122 string form matching fwupd / sysfs esrt class. */
    put_hex32(g->d1); putc('-');
    put_hex16(g->d2); putc('-');
    put_hex16(g->d3); putc('-');
    put_hex8(g->d4[0]); put_hex8(g->d4[1]); putc('-');
    for (int i = 2; i < 8; i++) put_hex8(g->d4[i]);
}

static int guid_eq(const guid *a, u32 d1, u16 d2, u16 d3)
{
    return a->d1 == d1 && a->d2 == d2 && a->d3 == d3;
}

static void *config(system_table *st, u32 d1, u16 d2, u16 d3)
{
    for (u64 i = 0; i < st->ntables; i++)
        if (guid_eq(&st->tables[i].id, d1, d2, d3)) return st->tables[i].table;
    return 0;
}

static void copy_cap(u8 *dst, const u8 *src, u32 n)
{
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static void dump_esrt(esrt_table *esrt)
{
    puts("APP esrt-list count=");
    put_u32(esrt ? esrt->fw_resource_count : 0);
    puts(" version=");
    put_u32(esrt ? (u32)esrt->fw_resource_version : 0);
    puts(" max=");
    put_u32(esrt ? esrt->fw_resource_count_max : 0);
    puts("\n");
    if (!esrt || esrt->fw_resource_count < 1) return;
    esrt_entry *e = &esrt->entries[0];
    puts("APP esrt-entry0 class=");
    put_guid(&e->fw_class);
    puts(" type=");
    put_u32(e->fw_type);
    puts(" fw=0x");
    put_hex32(e->fw_version);
    puts(" lowest=0x");
    put_hex32(e->lowest_supported_fw_version);
    puts(" flags=0x");
    put_hex32(e->capsule_flags);
    puts(" last_ver=0x");
    put_hex32(e->last_attempt_version);
    puts(" last_status=");
    put_u32(e->last_attempt_status);
    puts("\n");
}

#define ESRT_D1 0xb122a263u
#define ESRT_D2 0x3661
#define ESRT_D3 0x4f68
#define SYSFW_D1 0xa7c4e9f2u
#define SYSFW_D2 0x3b1d
#define SYSFW_D3 0x4e8a
#define TARGET_VER 0x00020300u
#define PERSIST_FLAG 0x00010000u

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    esrt_table *esrt = config(st, ESRT_D1, ESRT_D2, ESRT_D3);
    int list_ok = esrt && esrt->fw_resource_version == 1 &&
                  esrt->fw_resource_count >= 1 &&
                  esrt->fw_resource_count_max >= 1 &&
                  esrt->entries[0].fw_type == 1 &&
                  (esrt->entries[0].capsule_flags & PERSIST_FLAG) != 0 &&
                  guid_eq(&esrt->entries[0].fw_class, SYSFW_D1, SYSFW_D2, SYSFW_D3);

    dump_esrt(esrt);

    static u8 buf[8192];
    int update_ok = 0;
    if (capsule_project_blob_len <= sizeof buf) {
        copy_cap(buf, capsule_project_blob, capsule_project_blob_len);
        capsule_header *cap = (capsule_header *)buf;
        capsule_header *arr[1] = { cap };
        status us = st->rt->update_capsule(arr, 1, 0);
        update_ok = us == 0;
    }

    dump_esrt(esrt);

    int esrt_ok = list_ok && update_ok && esrt &&
                  esrt->entries[0].fw_version == TARGET_VER &&
                  esrt->entries[0].last_attempt_version == TARGET_VER &&
                  esrt->entries[0].last_attempt_status == 0;

    puts("APP capsule-key list=");
    puts(list_ok ? "ok" : "bad");
    puts(" update=");
    puts(update_ok ? "ok" : "bad");
    puts(" esrt=");
    puts(esrt_ok ? "ok" : "bad");
    if (list_ok && update_ok && esrt_ok)
        puts("\n");
    else
        puts(" =bad\n");
    return 0;
}
