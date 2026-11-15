/* SquirrelBIOS x86_64-uefi: a command line and an initrd for the image
 * the firmware boots. Original code.
 *
 *   -fw_cfg name=opt/org.squirrelworks/cmdline,string="console=ttyS0 ..."
 *       becomes the booted image's LoadOptions (UCS-2), which is where a
 *       Linux EFI stub (and most loaders) read their command line.
 *   -fw_cfg name=opt/org.squirrelworks/initrd,file=initrd.img
 *       is served through EFI_LOAD_FILE2_PROTOCOL on a handle whose
 *       device path is the vendor media node Linux looks for
 *       (LINUX_EFI_INITRD_MEDIA_GUID, Documentation/x86/boot.rst), so
 *       the kernel's stub can fetch it with LocateDevicePath. */
#include "efi.h"

#define FWCFG_CMDLINE "opt/org.squirrelworks/cmdline"
#define FWCFG_INITRD  "opt/org.squirrelworks/initrd"

static const EFI_GUID load_file2_guid =
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };
static const EFI_GUID initrd_media_guid =
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } };

typedef struct EFI_LOAD_FILE2_PROTOCOL {
    EFI_STATUS (EFIAPI *load_file)(struct EFI_LOAD_FILE2_PROTOCOL *, void *, BOOLEAN,
                                   UINTN *, void *);
} EFI_LOAD_FILE2_PROTOCOL;

static u16 initrd_sel;
static u32 initrd_size;

static EFI_STATUS EFIAPI initrd_load(EFI_LOAD_FILE2_PROTOCOL *this, void *path,
                                     BOOLEAN boot_policy, UINTN *size, void *buf)
{
    (void)this;
    (void)path;
    if (boot_policy)
        return EFI_UNSUPPORTED;         /* LoadFile2 never takes it */
    if (!size)
        return EFI_INVALID_PARAMETER;
    if (!buf || *size < initrd_size) {
        *size = initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    fwcfg_read(initrd_sel, buf, initrd_size);
    *size = initrd_size;
    return EFI_SUCCESS;
}

static EFI_LOAD_FILE2_PROTOCOL initrd_lf2 = { initrd_load };

/* VenMedia(LINUX_EFI_INITRD_MEDIA_GUID) and the end node */
static struct __attribute__((packed)) {
    u8 type, subtype, len[2];
    EFI_GUID guid;
    u8 end[4];
} initrd_dp = {
    0x04, 0x03, { 20, 0 }, { 0 }, { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0 }
};

void efi_initrd_init(void)
{
    if (!fwcfg_find(FWCFG_INITRD, &initrd_sel, &initrd_size) || !initrd_size)
        return;
    initrd_dp.guid = initrd_media_guid;
    EFI_HANDLE h = NULL;
    if (efi_install_protocol_interface(&h, (EFI_GUID *)&efi_device_path_guid, 0, &initrd_dp) ||
        efi_install_protocol_interface(&h, (EFI_GUID *)&load_file2_guid, 0, &initrd_lf2)) {
        printf("efi: could not publish the initrd\n");
        return;
    }
    printf("efi: initrd of %u bytes offered through LoadFile2\n", initrd_size);
}

void efi_set_load_options(EFI_HANDLE image)
{
    u16 sel;
    u32 size;
    if (!fwcfg_find(FWCFG_CMDLINE, &sel, &size) || !size || size > 4096)
        return;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (efi_handle_protocol(image, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li))
        return;
    u8 *text;
    CHAR16 *opt;
    if (efi_allocate_pool(EfiBootServicesData, size + 1, (void **)&text))
        return;
    if (efi_allocate_pool(EfiLoaderData, (size + 1) * 2, (void **)&opt)) {
        efi_free_pool(text);
        return;
    }
    fwcfg_read(sel, text, size);
    u32 n = 0;
    for (u32 i = 0; i < size && text[i]; i++)
        opt[n++] = text[i];             /* ASCII command lines */
    opt[n] = 0;
    efi_free_pool(text);
    li->load_options = opt;
    li->load_options_size = (n + 1) * 2;
    printf("efi: load options from fw_cfg (%u characters)\n", n);
}
