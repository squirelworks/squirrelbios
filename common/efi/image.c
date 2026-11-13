/* SquirrelBIOS UEFI core (common/efi): PE32+ images. LoadImage, StartImage, Exit
 * and UnloadImage. Original code, written from the PE/COFF format's
 * published description and the UEFI specification.
 *
 * An image is loaded from a buffer: headers and sections are copied to
 * pages of the image's code type, the rest of the image is zeroed, and
 * base relocations (DIR64, HIGHLOW) are applied when it does not land at
 * its preferred base. StartImage calls the entry point through the
 * architecture's image_call (arch.h), which keeps the stack pointer that
 * Exit returns to. The image's machine type must be ARCH_PE_MACHINE. */
#include "efi.h"

#define IMAGE_MAGIC 0x6567616d49517153ULL       /* "SqQImage" */

struct image {
    EFI_LOADED_IMAGE_PROTOCOL li;       /* first: the interface pointer is the record */
    u64 magic;
    EFI_HANDLE handle;
    u64 entry;
    u64 pages;
    int started;
    int running;
    u64 saved_sp;
    u16 subsystem;
    EFI_STATUS exit_status;
    UINTN exit_data_size;
    CHAR16 *exit_data;
};

EFI_HANDLE efi_firmware_handle;
static struct image firmware_image;
static struct image *current;

extern char __fw_start[], __fw_end[];


static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }

static struct image *image_of(EFI_HANDLE h)
{
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (!h || efi_handle_protocol(h, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li) != EFI_SUCCESS)
        return NULL;
    struct image *img = (struct image *)li;
    return img->magic == IMAGE_MAGIC ? img : NULL;
}

/* Copy and relocate the PE32+ image in buf; fill in img. */
static EFI_STATUS pe_load(const u8 *buf, UINTN size, struct image *img)
{
    if (size < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
        return EFI_LOAD_ERROR;
    u32 pe = rd32(buf + 0x3C);
    if ((u64)pe + 24 > size || memcmp(buf + pe, "PE\0\0", 4))
        return EFI_LOAD_ERROR;
    const u8 *coff = buf + pe + 4;
    if (rd16(coff) != ARCH_PE_MACHINE)
        return EFI_UNSUPPORTED;                 /* not for this CPU */
    u16 nsect = rd16(coff + 2);
    u16 optsize = rd16(coff + 16);
    u16 chars = rd16(coff + 18);
    const u8 *opt = coff + 20;
    if ((u64)(opt - buf) + optsize > size || optsize < 112 || rd16(opt) != 0x20B)
        return EFI_LOAD_ERROR;                  /* not PE32+ */
    u32 entry = rd32(opt + 16);
    u64 pref = rd64(opt + 24);
    u32 sect_align = rd32(opt + 32);
    u32 image_size = rd32(opt + 56);
    u32 hdr_size = rd32(opt + 60);
    u16 subsystem = rd16(opt + 68);
    u32 ndirs = rd32(opt + 108);
    u32 reloc_rva = 0, reloc_size = 0;
    if (ndirs > 5 && optsize >= 112 + 6 * 8) {
        reloc_rva = rd32(opt + 112 + 5 * 8);
        reloc_size = rd32(opt + 112 + 5 * 8 + 4);
    }
    const u8 *sect = opt + optsize;
    if ((u64)(sect - buf) + (u64)nsect * 40 > size || image_size == 0 || hdr_size > size ||
        hdr_size > image_size || entry >= image_size)
        return EFI_LOAD_ERROR;

    EFI_MEMORY_TYPE code = EfiLoaderCode, data = EfiLoaderData;
    if (subsystem == EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER)
        code = EfiBootServicesCode, data = EfiBootServicesData;
    else if (subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER)
        code = EfiRuntimeServicesCode, data = EfiRuntimeServicesData;
    else if (subsystem != EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION)
        return EFI_UNSUPPORTED;

    u64 pages = ((u64)image_size + 4095) / 4096;
    /* Sections must land on their alignment: place the image on it when
     * it is more than a page (64 KB for an arm64 Linux Image) */
    u64 align = (sect_align > 4096 && !(sect_align & (sect_align - 1)) && sect_align <= 0x200000)
                ? sect_align : 4096;
    EFI_PHYSICAL_ADDRESS base = pref;
    if ((pref & (align - 1)) || efi_allocate_pages(AllocateAddress, code, pages, &base) != EFI_SUCCESS) {
        if (chars & 0x0001)                     /* relocations stripped */
            return EFI_LOAD_ERROR;
        u64 extra = align / 4096 - 1;
        if (efi_allocate_pages(AllocateAnyPages, code, pages + extra, &base) != EFI_SUCCESS)
            return EFI_OUT_OF_RESOURCES;
        u64 aligned = (base + align - 1) & ~(align - 1);
        u64 head = (aligned - base) / 4096;
        if (head)
            efi_free_pages(base, head);
        if (extra - head)
            efi_free_pages(aligned + pages * 4096, extra - head);
        base = aligned;
    }
    u8 *mem = (u8 *)(uptr)base;
    memset(mem, 0, pages * 4096);
    memcpy(mem, buf, hdr_size);
    for (u16 i = 0; i < nsect; i++) {
        const u8 *s = sect + i * 40;
        u32 vsize = rd32(s + 8), va = rd32(s + 12), rawsize = rd32(s + 16), raw = rd32(s + 20);
        u32 n = rawsize < vsize ? rawsize : vsize;
        if (vsize == 0)
            n = rawsize;
        if ((u64)va + (vsize > n ? vsize : n) > image_size || (n && (u64)raw + n > size))
            goto bad;
        memcpy(mem + va, buf + raw, n);
    }

    s64 delta = (s64)(base - pref);
    if (delta && reloc_size) {
        if ((u64)reloc_rva + reloc_size > image_size)
            goto bad;
        u32 off = 0;
        while (off + 8 <= reloc_size) {
            const u8 *blk = mem + reloc_rva + off;
            u32 page = rd32(blk), bsize = rd32(blk + 4);
            if (bsize < 8 || off + bsize > reloc_size)
                goto bad;
            for (u32 j = 8; j + 2 <= bsize; j += 2) {
                u16 e = rd16(blk + j);
                u32 at = page + (e & 0xFFF);
                switch (e >> 12) {
                case 0:                         /* padding */
                    break;
                case 3:                         /* HIGHLOW */
                    if ((u64)at + 4 > image_size)
                        goto bad;
                    *(u32 *)(mem + at) += (u32)delta;
                    break;
                case 10:                        /* DIR64 */
                    if ((u64)at + 8 > image_size)
                        goto bad;
                    *(u64 *)(mem + at) += (u64)delta;
                    break;
                default:
                    goto bad;
                }
            }
            off += bsize;
        }
    }

    arch_sync_icache(mem, pages * 4096);
    img->li.revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    img->li.system_table = efi_st;
    img->li.image_base = mem;
    img->li.image_size = image_size;
    img->li.image_code_type = code;
    img->li.image_data_type = data;
    img->entry = base + entry;
    img->pages = pages;
    img->subsystem = subsystem;
    return EFI_SUCCESS;
bad:
    efi_free_pages(base, pages);
    return EFI_LOAD_ERROR;
}

EFI_STATUS EFIAPI efi_load_image(BOOLEAN boot_policy, EFI_HANDLE parent, void *path,
                                 void *src, UINTN size, EFI_HANDLE *out)
{
    (void)boot_policy;
    (void)path;
    if (!out || !image_of(parent))
        return EFI_INVALID_PARAMETER;
    if (!src)
        return EFI_NOT_FOUND;                   /* loading from a device comes later */
    EFI_STATUS v = arch_check_image(src, size);    /* x86-64: Secure Boot */
    if (v != EFI_SUCCESS)
        return v;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct image), &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    struct image *img = mem;
    memset(img, 0, sizeof *img);
    EFI_STATUS s = pe_load(src, size, img);
    if (s != EFI_SUCCESS) {
        efi_free_pool(img);
        return s;
    }
    img->magic = IMAGE_MAGIC;
    img->li.parent_handle = parent;
    EFI_HANDLE h = NULL;
    s = efi_install_protocol_interface(&h, (EFI_GUID *)&efi_loaded_image_guid, 0, &img->li);
    if (s != EFI_SUCCESS) {
        efi_free_pages((uptr)img->li.image_base, img->pages);
        efi_free_pool(img);
        return s;
    }
    img->handle = h;
    *out = h;
    return EFI_SUCCESS;
}

static void image_free(struct image *img)
{
    efi_uninstall_protocol_interface(img->handle, (EFI_GUID *)&efi_loaded_image_guid, &img->li);
    efi_free_pages((uptr)img->li.image_base, img->pages);
    img->magic = 0;
    efi_free_pool(img);
}

EFI_STATUS EFIAPI efi_start_image(EFI_HANDLE h, UINTN *exit_size, CHAR16 **exit_data)
{
    struct image *img = image_of(h);
    if (!img || img == &firmware_image || img->started)
        return EFI_INVALID_PARAMETER;
    img->started = 1;
    img->running = 1;
    struct image *caller = current;
    current = img;
    EFI_STATUS s = image_call(img->entry, h, efi_st, &img->saved_sp);
    current = caller;
    img->running = 0;
    if (exit_size)
        *exit_size = img->exit_data_size;
    if (exit_data)
        *exit_data = img->exit_data;
    if (img->subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION || s != EFI_SUCCESS)
        image_free(img);                /* applications and failed drivers go */
    return s;
}

static EFI_STATUS EFIAPI efi_exit(EFI_HANDLE h, EFI_STATUS status, UINTN size, CHAR16 *data)
{
    struct image *img = image_of(h);
    if (!img)
        return EFI_INVALID_PARAMETER;
    if (!img->started) {                /* loaded, never started: just unload */
        image_free(img);
        return EFI_SUCCESS;
    }
    if (!img->running || img != current)
        return EFI_INVALID_PARAMETER;
    img->exit_data_size = data ? size : 0;
    img->exit_data = data;
    image_exit(img->saved_sp, status);
}

EFI_STATUS EFIAPI efi_unload_image(EFI_HANDLE h)
{
    struct image *img = image_of(h);
    if (!img || img == &firmware_image || img->running)
        return EFI_INVALID_PARAMETER;
    if (img->started) {
        if (!img->li.unload)
            return EFI_UNSUPPORTED;
        EFI_STATUS s = img->li.unload(h);
        if (s != EFI_SUCCESS)
            return s;
    }
    image_free(img);
    return EFI_SUCCESS;
}

void efi_image_init(EFI_BOOT_SERVICES *bs)
{
    bs->load_image = (void *)efi_load_image;
    bs->start_image = (void *)efi_start_image;
    bs->exit = (void *)efi_exit;
    bs->unload_image = (void *)efi_unload_image;

    /* The firmware is an image too: parent of what it loads */
    firmware_image.magic = IMAGE_MAGIC;
    firmware_image.li.revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    firmware_image.li.system_table = efi_st;
    firmware_image.li.image_base = __fw_start;
    firmware_image.li.image_size = (u64)(__fw_end - __fw_start);
    firmware_image.li.image_code_type = EfiBootServicesCode;
    firmware_image.li.image_data_type = EfiBootServicesData;
    firmware_image.started = 1;
    firmware_image.running = 1;
    EFI_HANDLE h = NULL;
    efi_install_protocol_interface(&h, (EFI_GUID *)&efi_loaded_image_guid, 0, &firmware_image.li);
    firmware_image.handle = h;
    efi_firmware_handle = h;
}
