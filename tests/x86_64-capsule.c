/* UpdateCapsule + ESRT on x86_64-uefi (CONTINUANCE Phase G). Original code,
 * written from UEFI 2.10 §8.5.3 and §4.6. Built like tests/x86_64-ipmi.c.
 * Reports on COM1:
 *
 *   APP capsule esrt=ok query=ok update=ok attempt=ok osind=ok
 *
 * esrt     the EFI System Resource Table is among the configuration
 *          tables, version 1, one system-firmware resource whose FwClass
 *          is the SquirrelBIOS system-firmware GUID
 * query    QueryCapsuleCapabilities accepts a matching capsule header and
 *          reports a non-zero MaxCapsuleSize
 * update   UpdateCapsule accepts that capsule (no persist / reset flags)
 * attempt  ESRT LastAttemptStatus is SUCCESS and LastAttemptVersion matches
 *          the resource's FwVersion after UpdateCapsule
 * osind    GetVariable(OsIndicationsSupported) has the file-capsule bit
 *
 * or a field is "bad" when the check fails. Returns 0. */

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
    table_header hdr;
    void *vendor;
    u32 revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    void *con_out;
    handle stderr_handle;
    void *std_err;
    runtime_services *rt;
    boot_services *bs;
    u64 ntables;
    config_table *tables;
} system_table;

typedef struct {
    guid capsule_guid;
    u32 header_size;
    u32 flags;
    u32 capsule_image_size;
} capsule_header;

typedef struct {
    guid fw_class;
    u32 fw_type;
    u32 fw_version;
    u32 lowest_supported_fw_version;
    u32 capsule_flags;
    u32 last_attempt_version;
    u32 last_attempt_status;
} esrt_entry;

typedef struct {
    u32 fw_resource_count;
    u32 fw_resource_count_max;
    u64 fw_resource_version;
    esrt_entry entries[];
} esrt_table;

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

static int guid_eq(const guid *a, u32 d1, u16 d2, u16 d3)
{
    return a->d1 == d1 && a->d2 == d2 && a->d3 == d3;
}

static void *config(system_table *st, u32 d1, u16 d2, u16 d3)
{
    for (u64 i = 0; i < st->ntables; i++) {
        guid *g = &st->tables[i].id;
        if (guid_eq(g, d1, d2, d3))
            return st->tables[i].table;
    }
    return 0;
}

/* EFI_SYSTEM_RESOURCE_TABLE_GUID */
#define ESRT_D1 0xb122a263u
#define ESRT_D2 0x3661
#define ESRT_D3 0x4f68

/* SQUIRREL_SYSTEM_FIRMWARE_GUID */
#define SYSFW_D1 0xa7c4e9f2u
#define SYSFW_D2 0x3b1d
#define SYSFW_D3 0x4e8a

/* EFI_GLOBAL_VARIABLE */
#define GLOBAL_D1 0x8BE4DF61u
#define GLOBAL_D2 0x93CA
#define GLOBAL_D3 0x11d2

#define FILE_CAPSULE_BIT 0x4ULL

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)image;

    esrt_table *esrt = config(st, ESRT_D1, ESRT_D2, ESRT_D3);
    int esrt_ok = esrt && esrt->fw_resource_version == 1 && esrt->fw_resource_count >= 1 &&
                  esrt->entries[0].fw_type == 1 &&
                  guid_eq(&esrt->entries[0].fw_class, SYSFW_D1, SYSFW_D2, SYSFW_D3);

    capsule_header cap;
    for (u32 i = 0; i < sizeof cap; i++)
        ((u8 *)&cap)[i] = 0;
    cap.capsule_guid.d1 = SYSFW_D1;
    cap.capsule_guid.d2 = SYSFW_D2;
    cap.capsule_guid.d3 = SYSFW_D3;
    cap.capsule_guid.d4[0] = 0x9c;
    cap.capsule_guid.d4[1] = 0x55;
    cap.capsule_guid.d4[2] = 0x12;
    cap.capsule_guid.d4[3] = 0x84;
    cap.capsule_guid.d4[4] = 0x6f;
    cap.capsule_guid.d4[5] = 0x91;
    cap.capsule_guid.d4[6] = 0xde;
    cap.capsule_guid.d4[7] = 0x30;
    cap.header_size = (u32)sizeof cap;
    cap.flags = 0;
    cap.capsule_image_size = (u32)sizeof cap;

    capsule_header *arr[1] = { &cap };
    u64 max_size = 0;
    u32 reset_type = 0xFFFFFFFFu;
    status qs = st->rt->query_capsule_capabilities(arr, 1, &max_size, &reset_type);
    int query_ok = qs == 0 && max_size >= sizeof cap;

    /* Firmware may printf during UpdateCapsule; print the APP summary after. */
    status us = st->rt->update_capsule(arr, 1, 0);
    int update_ok = us == 0;

    int attempt_ok = esrt_ok && esrt->entries[0].last_attempt_status == 0 &&
                     esrt->entries[0].last_attempt_version == esrt->entries[0].fw_version;

    static u16 name[] = { 'O','s','I','n','d','i','c','a','t','i','o','n','s',
                          'S','u','p','p','o','r','t','e','d', 0 };
    guid gvar = { GLOBAL_D1, GLOBAL_D2, GLOBAL_D3,
                  { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
    u64 bits = 0;
    u64 bsize = sizeof bits;
    u32 attr = 0;
    status gs = st->rt->get_variable(name, &gvar, &attr, &bsize, &bits);
    int osind_ok = gs == 0 && (bits & FILE_CAPSULE_BIT) != 0;

    /* One line after any firmware UpdateCapsule printf, so smoke can
     * match the summary literally. */
    puts("APP capsule esrt=");
    puts(esrt_ok ? "ok" : "bad");
    puts(" query=");
    puts(query_ok ? "ok" : "bad");
    puts(" update=");
    puts(update_ok ? "ok" : "bad");
    puts(" attempt=");
    puts(attempt_ok ? "ok" : "bad");
    puts(" osind=");
    puts(osind_ok ? "ok" : "bad");
    if (esrt_ok && query_ok && update_ok && attempt_ok && osind_ok)
        puts("\n");
    else
        puts(" =bad\n");
    return 0;
}
