/* Signed capsule verify + apply / PersistAcrossReset staging (Phase G). Original. */
#include "capsule_good_blob.h"
#include "capsule_bad_blob.h"
#include "capsule_persist_blob.h"
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef u64 status; typedef void *handle;
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
typedef struct { u64 length, data_or_cont; } capsule_block;
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
static int guid_eq(const guid *a, u32 d1, u16 d2, u16 d3) {
    return a->d1 == d1 && a->d2 == d2 && a->d3 == d3;
}
static void *config(system_table *st, u32 d1, u16 d2, u16 d3) {
    for (u64 i = 0; i < st->ntables; i++)
        if (guid_eq(&st->tables[i].id, d1, d2, d3)) return st->tables[i].table;
    return 0;
}
static void copy_cap(u8 *dst, const u8 *src, u32 n) { for (u32 i = 0; i < n; i++) dst[i] = src[i]; }
#define ESRT_D1 0xb122a263u
#define ESRT_D2 0x3661
#define ESRT_D3 0x4f68
#define SYSFW_D1 0xa7c4e9f2u
#define SYSFW_D2 0x3b1d
#define SYSFW_D3 0x4e8a
#define TARGET_VER 0x00020300u
#define AUTH_ERROR 5u
#define EFI_SECURITY_VIOLATION 0x800000000000001aULL
#define EFI_BUFFER_TOO_SMALL   0x8000000000000005ULL
EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;
    esrt_table *esrt = config(st, ESRT_D1, ESRT_D2, ESRT_D3);
    int esrt_ok = esrt && esrt->fw_resource_count >= 1;
    static u8 good_buf[8192], bad_buf[8192], persist_buf[8192];
    if (capsule_good_blob_len > sizeof good_buf || capsule_bad_blob_len > sizeof bad_buf ||
        capsule_persist_blob_len > sizeof persist_buf) return 1;
    copy_cap(good_buf, capsule_good_blob, capsule_good_blob_len);
    capsule_header *good = (capsule_header *)good_buf;
    capsule_header *garr[1] = { good };
    status us = st->rt->update_capsule(garr, 1, 0);
    int good_ok = us == 0 && esrt_ok && esrt->entries[0].last_attempt_status == 0 &&
                  esrt->entries[0].last_attempt_version == TARGET_VER;
    copy_cap(bad_buf, capsule_bad_blob, capsule_bad_blob_len);
    capsule_header *bad = (capsule_header *)bad_buf;
    capsule_header *barr[1] = { bad };
    status bs = st->rt->update_capsule(barr, 1, 0);
    int bad_ok = bs == EFI_SECURITY_VIOLATION && esrt_ok &&
                 esrt->entries[0].last_attempt_status == AUTH_ERROR;
    copy_cap(persist_buf, capsule_persist_blob, capsule_persist_blob_len);
    capsule_header *pers = (capsule_header *)persist_buf;
    capsule_header *parr[1] = { pers };
    static capsule_block sgl[2];
    sgl[0].length = pers->capsule_image_size;
    sgl[0].data_or_cont = (u64)(unsigned long)persist_buf;
    sgl[1].length = 0; sgl[1].data_or_cont = 0;
    status ss = st->rt->update_capsule(parr, 1, (u64)(unsigned long)sgl);
    static u16 stage_name[] = { 'S','q','C','a','p','s','u','l','e','S','t','a','g','i','n','g', 0 };
    guid sysfw = { SYSFW_D1, SYSFW_D2, SYSFW_D3, { 0x9c, 0x55, 0x12, 0x84, 0x6f, 0x91, 0xde, 0x30 } };
    u64 st_size = 0; u32 attr = 0;
    status gs = st->rt->get_variable(stage_name, &sysfw, &attr, &st_size, 0);
    int stage_ok = ss == 0 && gs == EFI_BUFFER_TOO_SMALL && st_size == pers->capsule_image_size;
    puts("APP capsule-signed good="); puts(good_ok ? "ok" : "bad");
    puts(" bad="); puts(bad_ok ? "ok" : "bad");
    puts(" stage="); puts(stage_ok ? "ok" : "bad");
    puts(" attempt="); puts(good_ok ? "ok" : "bad");
    puts((good_ok && bad_ok && stage_ok) ? "\n" : " =bad\n");
    return 0;
}
