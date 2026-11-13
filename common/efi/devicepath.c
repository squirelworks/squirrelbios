/* SquirrelBIOS UEFI core (common/efi): device path helpers. Original code. */
#include "efi.h"

const EFI_GUID efi_device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

UINTN efi_dp_size(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    UINTN n = 0;
    if (!dp)
        return 0;
    for (;;) {
        UINTN len = dp->length[0] | ((UINTN)dp->length[1] << 8);
        if (len < 4)
            return n;
        n += len;
        if (dp->type == END_DEVICE_PATH_TYPE && dp->subtype == END_ENTIRE_DEVICE_PATH_SUBTYPE)
            return n;
        dp = (const EFI_DEVICE_PATH_PROTOCOL *)((const u8 *)dp + len);
    }
}

static EFI_DEVICE_PATH_PROTOCOL *dp_end(void)
{
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, 4, &mem) != EFI_SUCCESS)
        return NULL;
    EFI_DEVICE_PATH_PROTOCOL *e = mem;
    e->type = END_DEVICE_PATH_TYPE;
    e->subtype = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    e->length[0] = 4;
    e->length[1] = 0;
    return e;
}

EFI_DEVICE_PATH_PROTOCOL *efi_dp_append(const EFI_DEVICE_PATH_PROTOCOL *a,
                                        const EFI_DEVICE_PATH_PROTOCOL *b)
{
    UINTN as = a ? efi_dp_size(a) : 4;
    UINTN bs = b ? efi_dp_size(b) : 4;
    if (a)
        as -= 4;                        /* drop a's end node */
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, as + bs, &mem) != EFI_SUCCESS)
        return NULL;
    u8 *p = mem;
    if (a)
        memcpy(p, a, as);
    if (b)
        memcpy(p + as, b, bs);
    else {
        p[as] = END_DEVICE_PATH_TYPE;
        p[as + 1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
        p[as + 2] = 4;
        p[as + 3] = 0;
    }
    return mem;
}

EFI_DEVICE_PATH_PROTOCOL *efi_dp_file(const CHAR16 *path)
{
    UINTN n = 0;
    while (path[n])
        n++;
    UINTN bytes = 4 + (n + 1) * sizeof(CHAR16);
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, bytes + 4, &mem) != EFI_SUCCESS)
        return NULL;
    EFI_DEVICE_PATH_PROTOCOL *dp = mem;
    dp->type = MEDIA_DEVICE_PATH;
    dp->subtype = MEDIA_FILEPATH_DP;
    dp->length[0] = (u8)bytes;
    dp->length[1] = (u8)(bytes >> 8);
    memcpy(dp + 1, path, (n + 1) * sizeof(CHAR16));
    EFI_DEVICE_PATH_PROTOCOL *e = (EFI_DEVICE_PATH_PROTOCOL *)((u8 *)mem + bytes);
    e->type = END_DEVICE_PATH_TYPE;
    e->subtype = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    e->length[0] = 4;
    e->length[1] = 0;
    (void)dp_end;
    return dp;
}

/* PCI device path node: Type 1 SubType 1, length 6, Device, Function. */
EFI_DEVICE_PATH_PROTOCOL *efi_dp_pci(u8 device, u8 function)
{
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, 6 + 4, &mem) != EFI_SUCCESS)
        return NULL;
    u8 *p = mem;
    p[0] = HARDWARE_DEVICE_PATH;
    p[1] = HW_PCI_DP;
    p[2] = 6;
    p[3] = 0;
    p[4] = function;
    p[5] = device;
    p[6] = END_DEVICE_PATH_TYPE;
    p[7] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[8] = 4;
    p[9] = 0;
    return mem;
}

/* Hard-drive media node for a GPT partition. */
EFI_DEVICE_PATH_PROTOCOL *efi_dp_hd(u32 part, u64 start, u64 size, const u8 uuid[16])
{
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, 42 + 4, &mem) != EFI_SUCCESS)
        return NULL;
    u8 *p = mem;
    p[0] = MEDIA_DEVICE_PATH;
    p[1] = MEDIA_HARDDRIVE_DP;
    p[2] = 42;
    p[3] = 0;
    memcpy(p + 4, &part, 4);
    memcpy(p + 8, &start, 8);
    memcpy(p + 16, &size, 8);
    memcpy(p + 24, uuid, 16);
    p[40] = 0x02;                       /* GPT */
    p[41] = 0x02;                       /* signature type: GUID */
    p[42] = END_DEVICE_PATH_TYPE;
    p[43] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[44] = 4;
    p[45] = 0;
    return mem;
}
