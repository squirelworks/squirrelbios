/* SquirrelBIOS x86_64-uefi: UEFI interface definitions.
 * Original code, written from the UEFI 2.x specification's description
 * of the tables and calling convention (x64: the Microsoft ABI). */
#ifndef EFI_H
#define EFI_H

#include "sq.h"

#define EFIAPI __attribute__((ms_abi))

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef u64 EFI_PHYSICAL_ADDRESS;
typedef u64 EFI_VIRTUAL_ADDRESS;
typedef u16 CHAR16;
typedef u8 BOOLEAN;
typedef usize UINTN;
typedef UINTN EFI_TPL;

typedef struct {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
} EFI_GUID;

#define EFI_ERR(n) (0x8000000000000000ULL | (n))
#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            EFI_ERR(1)
#define EFI_INVALID_PARAMETER     EFI_ERR(2)
#define EFI_UNSUPPORTED           EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE       EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL      EFI_ERR(5)
#define EFI_NOT_READY             EFI_ERR(6)
#define EFI_DEVICE_ERROR          EFI_ERR(7)
#define EFI_OUT_OF_RESOURCES      EFI_ERR(9)
#define EFI_NOT_FOUND             EFI_ERR(14)
#define EFI_ACCESS_DENIED         EFI_ERR(15)

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} EFI_TABLE_HEADER;

#define EFI_SPEC_REVISION ((2 << 16) | 70)      /* 2.70 */
#define EFI_SYSTEM_TABLE_SIGNATURE   0x5453595320494249ULL  /* "IBI SYST" */
#define EFI_BOOT_SERVICES_SIGNATURE  0x56524553544f4f42ULL  /* "BOOTSERV" */
#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544e5552ULL /* "RUNTSERV" */

/* Memory */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

#define EFI_MEMORY_UC       0x0000000000000001ULL
#define EFI_MEMORY_WC       0x0000000000000002ULL
#define EFI_MEMORY_WT       0x0000000000000004ULL
#define EFI_MEMORY_WB       0x0000000000000008ULL
#define EFI_MEMORY_RUNTIME  0x8000000000000000ULL

typedef struct {
    u32 type;
    u32 pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    EFI_VIRTUAL_ADDRESS virtual_start;
    u64 number_of_pages;
    u64 attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_MEMORY_DESCRIPTOR_VERSION 1
#define EFI_PAGE_SIZE 4096

typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

typedef EFI_STATUS (EFIAPI *EFI_UNSUPPORTED_FN)(void);

typedef struct {
    EFI_TABLE_HEADER hdr;
    EFI_TPL (EFIAPI *raise_tpl)(EFI_TPL);
    void (EFIAPI *restore_tpl)(EFI_TPL);
    EFI_STATUS (EFIAPI *allocate_pages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *free_pages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *get_memory_map)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, u32 *);
    EFI_STATUS (EFIAPI *allocate_pool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *free_pool)(void *);
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    void *handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    void *exit_boot_services;
    void *get_next_monotonic_count;
    EFI_STATUS (EFIAPI *stall)(UINTN);
    void *set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    void *locate_protocol;
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    EFI_STATUS (EFIAPI *calculate_crc32)(void *, UINTN, u32 *);
    void (EFIAPI *copy_mem)(void *, void *, UINTN);
    void (EFIAPI *set_mem)(void *, UINTN, u8);
    void *create_event_ex;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER hdr;
    void *get_time;
    void *set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    void *get_variable;
    void *get_next_variable_name;
    void *set_variable;
    void *get_next_high_monotonic_count;
    void *reset_system;
    void *update_capsule;
    void *query_capsule_capabilities;
    void *query_variable_info;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    u32 firmware_revision;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    void *con_out;
    EFI_HANDLE standard_error_handle;
    void *std_err;
    EFI_RUNTIME_SERVICES *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    UINTN number_of_table_entries;
    EFI_CONFIGURATION_TABLE *configuration_table;
} EFI_SYSTEM_TABLE;

/* memory.c */
void efi_memory_init(void);
int efi_memory_selftest(void);
EFI_STATUS EFIAPI efi_allocate_pages(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
EFI_STATUS EFIAPI efi_free_pages(EFI_PHYSICAL_ADDRESS, UINTN);
EFI_STATUS EFIAPI efi_get_memory_map(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, u32 *);
EFI_STATUS EFIAPI efi_allocate_pool(EFI_MEMORY_TYPE, UINTN, void **);
EFI_STATUS EFIAPI efi_free_pool(void *);

/* tables.c */
extern EFI_SYSTEM_TABLE *efi_st;
void efi_tables_init(void);
u32 crc32(const void *buf, usize len);

#endif
