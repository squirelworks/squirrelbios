/* Capsule flash rewrite via NV identity (Phase G M2). Original code.
 *
 * Boot 1 (no SqFwFlashId yet): apply a signed SQCP capsule and expect
 * ESRT FwVersion to become TARGET_VER.
 * Boot 2 (same pflash vars store): firmware loads SqFwFlashId; this app
 * only checks that FwVersion is still TARGET_VER without calling
 * UpdateCapsule.
 *
 * Reports on COM1:
 *   APP capsule-flash apply=ok fw=0x00020300   (boot 1)
 *   APP capsule-flash kept=ok fw=0x00020300    (boot 2)
 */
#include "capsule_good_blob.h"

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
static void put_hex32(u32 v)
{
    static const char *h = "0123456789abcdef";
    putc('0'); putc('x');
    for (int i = 28; i >= 0; i -= 4)
        putc(h[(v >> i) & 0xf]);
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

#define ESRT_D1 0xb122a263u
#define ESRT_D2 0x3661
#define ESRT_D3 0x4f68
#define TARGET_VER 0x00020300u

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    esrt_table *esrt = config(st, ESRT_D1, ESRT_D2, ESRT_D3);
    if (!esrt || esrt->fw_resource_count < 1) {
        puts("APP capsule-flash esrt=bad\n");
        return 1;
    }
    u32 before = esrt->entries[0].fw_version;
    int applied = 0;
    if (before != TARGET_VER) {
        static u8 good_buf[8192];
        if (capsule_good_blob_len > sizeof good_buf) {
            puts("APP capsule-flash blob=bad\n");
            return 1;
        }
        copy_cap(good_buf, capsule_good_blob, capsule_good_blob_len);
        capsule_header *good = (capsule_header *)good_buf;
        capsule_header *arr[1] = { good };
        status us = st->rt->update_capsule(arr, 1, 0);
        applied = (us == 0);
    }
    u32 after = esrt->entries[0].fw_version;
    int fw_ok = after == TARGET_VER;
    int all = fw_ok && (before == TARGET_VER || applied);
    puts("APP capsule-flash ");
    if (before == TARGET_VER)
        puts("kept=");
    else
        puts("apply=");
    puts(all ? "ok" : "bad");
    puts(" fw=");
    put_hex32(after);
    puts(all ? "\n" : " =bad\n");
    return 0;
}
