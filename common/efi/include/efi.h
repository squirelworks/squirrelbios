/* SquirrelBIOS UEFI core (common/efi): UEFI interface definitions.
 * Original code, written from the UEFI 2.x specification's description
 * of the tables and calling conventions: the Microsoft ABI on x64, the
 * standard procedure call standard on AArch64 and RISC-V. */
#ifndef EFI_H
#define EFI_H

#include "sq.h"

#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#define efi_va_list  __builtin_ms_va_list
#define efi_va_start __builtin_ms_va_start
#define efi_va_end   __builtin_ms_va_end
#else
#define EFIAPI
#define efi_va_list  __builtin_va_list
#define efi_va_start __builtin_va_start
#define efi_va_end   __builtin_va_end
#endif

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
#define EFI_WRITE_PROTECTED       EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES      EFI_ERR(9)
#define EFI_VOLUME_CORRUPTED      EFI_ERR(10)
#define EFI_NO_MEDIA              EFI_ERR(12)
#define EFI_MEDIA_CHANGED         EFI_ERR(13)
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

#define EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION          10
#define EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER  11
#define EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER       12

#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL 0x04

typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

typedef struct {
    u32 revision;
    EFI_HANDLE parent_handle;
    void *system_table;
    EFI_HANDLE device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    EFI_MEMORY_TYPE image_code_type;
    EFI_MEMORY_TYPE image_data_type;
    EFI_STATUS (EFIAPI *unload)(EFI_HANDLE);
} EFI_LOADED_IMAGE_PROTOCOL;

extern const EFI_GUID efi_loaded_image_guid;
int guid_eq(const EFI_GUID *a, const EFI_GUID *b);

/* Events and timers */
#define EVT_TIMER                         0x80000000u
#define EVT_RUNTIME                       0x40000000u
#define EVT_NOTIFY_WAIT                   0x00000100u
#define EVT_NOTIFY_SIGNAL                 0x00000200u
#define EVT_SIGNAL_EXIT_BOOT_SERVICES     0x00000201u
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE 0x60000202u

typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;
typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT, void *);

#define TPL_APPLICATION 4
#define TPL_CALLBACK    8
#define TPL_NOTIFY      16
#define TPL_HIGH_LEVEL  31

/* Console */
typedef struct {
    u16 scan_code;
    CHAR16 unicode_char;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *read_key_stroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT wait_for_key;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    s32 max_mode;
    s32 mode;
    s32 attribute;
    s32 cursor_column;
    s32 cursor_row;
    BOOLEAN cursor_visible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *output_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    EFI_STATUS (EFIAPI *test_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    EFI_STATUS (EFIAPI *query_mode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *set_mode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *set_attribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *clear_screen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    EFI_STATUS (EFIAPI *set_cursor_position)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN);
    EFI_STATUS (EFIAPI *enable_cursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    SIMPLE_TEXT_OUTPUT_MODE *mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

#define EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID \
    { 0x387477c1, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID \
    { 0x387477c2, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Device path */
typedef struct {
    u8 type;
    u8 subtype;
    u8 length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define END_DEVICE_PATH_TYPE            0x7F
#define END_ENTIRE_DEVICE_PATH_SUBTYPE  0xFF
#define HARDWARE_DEVICE_PATH            0x01
#define HW_PCI_DP                       0x01
#define ACPI_DEVICE_PATH                0x02
#define ACPI_DP                         0x01
#define MESSAGING_DEVICE_PATH           0x03
#define MSG_SCSI_DP                     0x02
#define MEDIA_DEVICE_PATH               0x04
#define MEDIA_HARDDRIVE_DP              0x01
#define MEDIA_FILEPATH_DP               0x04

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Block I/O */
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_BLOCK_IO_PROTOCOL_REVISION  0x00010000

typedef struct {
    u64 media_id;
    BOOLEAN removable_media;
    BOOLEAN media_present;
    BOOLEAN logical_partition;
    BOOLEAN read_only;
    BOOLEAN write_caching;
    u32 block_size;
    u32 io_align;
    u64 last_block;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    u64 revision;
    EFI_BLOCK_IO_MEDIA *media;
    EFI_STATUS (EFIAPI *reset)(struct EFI_BLOCK_IO_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *read_blocks)(struct EFI_BLOCK_IO_PROTOCOL *, u64, u64, UINTN, void *);
    EFI_STATUS (EFIAPI *write_blocks)(struct EFI_BLOCK_IO_PROTOCOL *, u64, u64, UINTN, void *);
    EFI_STATUS (EFIAPI *flush_blocks)(struct EFI_BLOCK_IO_PROTOCOL *);
} EFI_BLOCK_IO_PROTOCOL;

/* Simple File System / File */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION 0x00010000
#define EFI_FILE_PROTOCOL_REVISION 0x00010000

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_READ_ONLY   0x0000000000000001ULL
#define EFI_FILE_DIRECTORY   0x0000000000000010ULL

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    u64 size;
    u64 file_size;
    u64 physical_size;
    u16 create_time[8];
    u16 last_access_time[8];
    u16 modification_time[8];
    u64 attribute;
    CHAR16 file_name[1];
} EFI_FILE_INFO;

typedef struct EFI_FILE_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, u64, u64);
    EFI_STATUS (EFIAPI *close)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *delete)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *write)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *get_position)(struct EFI_FILE_PROTOCOL *, u64 *);
    EFI_STATUS (EFIAPI *set_position)(struct EFI_FILE_PROTOCOL *, u64);
    EFI_STATUS (EFIAPI *get_info)(struct EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *set_info)(struct EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *flush)(struct EFI_FILE_PROTOCOL *);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *open_volume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

extern const EFI_GUID efi_device_path_guid;
extern const EFI_GUID efi_block_io_guid;
extern const EFI_GUID efi_simple_file_system_guid;
extern const EFI_GUID efi_file_info_guid;

UINTN efi_dp_size(const EFI_DEVICE_PATH_PROTOCOL *dp);
EFI_DEVICE_PATH_PROTOCOL *efi_dp_append(const EFI_DEVICE_PATH_PROTOCOL *a,
                                        const EFI_DEVICE_PATH_PROTOCOL *b);
EFI_DEVICE_PATH_PROTOCOL *efi_dp_file(const CHAR16 *path);
EFI_DEVICE_PATH_PROTOCOL *efi_dp_pci(u8 device, u8 function);
EFI_DEVICE_PATH_PROTOCOL *efi_dp_hd(u32 part, u64 start, u64 size, const u8 uuid[16]);

/* disk.c / fat.c: virtio-blk Block I/O, GPT/MBR partitions, FAT SFS */
void efi_disk_init(void);
/* A disk a driver found, in 512-byte sectors; read and write take whole
 * sectors, return 1 on success. disk.c publishes it (efi_disk_add). */
struct sq_blk {
    u64 sectors;
    EFI_DEVICE_PATH_PROTOCOL *path;
    int (*read)(struct sq_blk *, u64 lba, void *buf, u32 bytes);
    int (*write)(struct sq_blk *, u64 lba, void *buf, u32 bytes);
};
void efi_disk_add(struct sq_blk *dev);

/* virtio.c: virtqueue and virtio-blk requests, any transport */
struct vq {
    u8 *mem;                    /* descriptors + avail, then used 4 KB on */
    u8 *req;                    /* 16-byte request header, status byte */
    u16 last_used;
    uptr notify;                /* written with the queue index (0) */
    int notify16;               /* a 16-bit write (PCI), else 32-bit */
};
#define VQ_SIZE     8
#define VQ_USED_OFF 4096
int vq_alloc(struct vq *q);
uptr vq_desc(struct vq *q);
uptr vq_avail(struct vq *q);
uptr vq_used(struct vq *q);
int virtio_blk_add(struct vq *q, u64 sectors, EFI_DEVICE_PATH_PROTOCOL *path);

/* virtio_mmio.c, pci.c (virtio_pci.c, nvme.c): disks found, each passed
 * to efi_disk_add; the count */
int virtio_mmio_probe(void);
int pci_probe(void);

/* A PCI function the platform found (pci.c: the device tree's ECAM host
 * bridge; x86_64/platform/pcidisk.c), its memory BARs assigned (CPU
 * addresses, 0 if unused) and memory decoding and bus mastering on */
struct sq_pci {
    u8 bus, dev, fn;
    u16 vendor, device;
    u32 class_rev;              /* class code << 8 | revision */
    u64 bar[6];                 /* a 64-bit BAR's address sits in its low slot */
    EFI_DEVICE_PATH_PROTOCOL *path;
    u32 (*read32)(struct sq_pci *p, u32 off);  /* configuration space */
    uptr priv;                  /* the platform's */
};
static inline u8 pci_cfg8(struct sq_pci *p, u32 off)
{
    return (u8)(p->read32(p, off & ~3u) >> (off & 3) * 8);
}
static inline u16 pci_cfg16(struct sq_pci *p, u32 off)
{
    return (u16)(p->read32(p, off & ~3u) >> (off & 2) * 8);
}
/* The drivers: each takes the function if it is theirs (1) */
int virtio_pci_add(struct sq_pci *p);   /* virtio_pci.c */
int nvme_add(struct sq_pci *p);         /* nvme.c */
int ahci_add(struct sq_pci *p);         /* ahci.c */
int xhci_add(struct sq_pci *p);         /* xhci.c: USB mass storage */
int sdhci_add(struct sq_pci *p);        /* sdhci.c: SD cards (PCI class 08 05) */
int sdhci_add_mmio(uptr regs, u32 base_mhz, EFI_DEVICE_PATH_PROTOCOL *path, const char *where);
int sdhci_probe_dt(void);               /* sdhci_dt.c: SD hosts in the device tree */
int pci_disk_add(struct sq_pci *p);     /* pci.c: whichever fits */
int efi_boot_esp(void);
int fat_install(EFI_HANDLE handle, EFI_BLOCK_IO_PROTOCOL *bio);

/* Graphics Output Protocol (Bochs DISPI / QEMU std VGA) */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 red_mask, green_mask, blue_mask, reserved_mask;
} EFI_PIXEL_BITMASK;

typedef struct {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    EFI_GRAPHICS_PIXEL_FORMAT pixel_format;
    EFI_PIXEL_BITMASK pixel_information;
    u32 pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 max_mode;
    u32 mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    UINTN frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    u8 blue, green, red, reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
    EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo,
    EfiBltVideoToVideo, EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *query_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32,
                                    UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (EFIAPI *set_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32);
    EFI_STATUS (EFIAPI *blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *,
                             EFI_GRAPHICS_OUTPUT_BLT_PIXEL *, EFI_GRAPHICS_OUTPUT_BLT_OPERATION,
                             UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

extern const EFI_GUID efi_gop_guid;
void efi_gop_init(void);

/* Runtime services and ExitBootServices */
typedef struct {
    u16 year;
    u8 month, day, hour, minute, second, pad1;
    u32 nanosecond;
    s16 timezone;
    u8 daylight, pad2;
} EFI_TIME;

typedef struct {
    u32 resolution, accuracy;
    BOOLEAN sets_to_zero;
} EFI_TIME_CAPABILITIES;

typedef enum {
    EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific
} EFI_RESET_TYPE;

void efi_runtime_init(EFI_RUNTIME_SERVICES *rt);
void efi_note_map_key(UINTN key);
EFI_STATUS EFIAPI efi_exit_boot_services(EFI_HANDLE image, UINTN map_key);
int efi_boot_services_exited(void);

/* event.c */
void efi_event_init(EFI_BOOT_SERVICES *bs);
void efi_timers_poll(void);
void efi_signal_exit_boot_services(void);
u64 efi_counter_hz(void);
EFI_STATUS EFIAPI efi_create_event(u32, EFI_TPL, EFI_EVENT_NOTIFY, void *, EFI_EVENT *);
EFI_STATUS EFIAPI efi_signal_event(EFI_EVENT);
EFI_STATUS EFIAPI efi_stall(UINTN);

/* textcon.c */
void efi_console_init(void);

/* protocol.c */
void efi_protocol_init(EFI_BOOT_SERVICES *bs);
EFI_STATUS EFIAPI efi_install_protocol_interface(EFI_HANDLE *, EFI_GUID *, u32, void *);
EFI_STATUS EFIAPI efi_uninstall_protocol_interface(EFI_HANDLE, EFI_GUID *, void *);
EFI_STATUS EFIAPI efi_handle_protocol(EFI_HANDLE, EFI_GUID *, void **);
EFI_STATUS EFIAPI efi_install_configuration_table(EFI_GUID *, void *);

/* image.c */
void efi_image_init(EFI_BOOT_SERVICES *bs);
EFI_STATUS EFIAPI efi_load_image(BOOLEAN, EFI_HANDLE, void *, void *, UINTN, EFI_HANDLE *);
EFI_STATUS EFIAPI efi_start_image(EFI_HANDLE, UINTN *, CHAR16 **);
EFI_STATUS EFIAPI efi_unload_image(EFI_HANDLE);
extern EFI_HANDLE efi_firmware_handle;

/* memory.c */
void efi_memory_init(void);
int efi_memory_selftest(void);
u64 efi_free_pages_count(void);
EFI_STATUS EFIAPI efi_allocate_pages(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
EFI_STATUS EFIAPI efi_free_pages(EFI_PHYSICAL_ADDRESS, UINTN);
EFI_STATUS EFIAPI efi_get_memory_map(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, u32 *);
EFI_STATUS EFIAPI efi_allocate_pool(EFI_MEMORY_TYPE, UINTN, void **);
EFI_STATUS EFIAPI efi_free_pool(void *);

/* tables.c */
extern EFI_SYSTEM_TABLE *efi_st;
void efi_tables_init(void);
void efi_reseal(EFI_TABLE_HEADER *h);
u32 crc32(const void *buf, usize len);

/* Device tree machines (main.c, memory.c, arch) */
#define EFI_DTB_TABLE_GUID \
    { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }
#define EFI_LOAD_FILE2_PROTOCOL_GUID \
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } }
#define LINUX_EFI_INITRD_MEDIA_GUID \
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }
void efi_main(const void *dtb) __attribute__((noreturn));
void efi_memory_mark(u64 start, u64 pages, u32 type);
void arch_init(void);           /* after the console and device tree: CPU, timer, interrupts */
void arch_efi_init(void);       /* after the EFI tables: arch protocols and tables */
void arch_virtual_map(void);
/* A -kernel image QEMU already put in RAM (RISC-V), or 0 */
int arch_kernel(void **image, u32 *size);    /* SetVirtualAddressMap: efi_convert_pointer its own pointers */
int efi_convert_pointer(void *pp);  /* u64 *: 1 when it moved */
void efi_memory_mark_attr(u64 start, u64 pages, u32 type, u64 attr);

/* common/efi/net_boot.c: the NICs, each with its Simple Network Protocol;
 * a PXE-style boot from them (1 if an image ran) */
void efi_net_init(void);
int efi_net_boot(void);

#endif
