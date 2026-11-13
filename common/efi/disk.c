/* SquirrelBIOS UEFI core (common/efi): Block I/O, GPT/MBR partitions,
 * ESP boot. Original code; started as a copy of x86_64/core/disk.c.
 * Publishes Block I/O and a device path for every disk a driver found
 * (virtio-mmio, virtio-pci, NVMe), walks GPT (or MBR) for partitions, mounts FAT
 * volumes, and starts the architecture's \EFI\BOOT\BOOT<arch>.EFI. */
#include "efi.h"

const EFI_GUID efi_block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const u8 esp_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

struct blk_dev {
    EFI_BLOCK_IO_PROTOCOL bio;
    EFI_BLOCK_IO_MEDIA media;
    EFI_HANDLE handle;
    EFI_DEVICE_PATH_PROTOCOL *path;
    struct sq_blk *dev;                 /* the driver's disk */
    /* Parent whole-disk (NULL for the disk itself); LBA offset into it. */
    struct blk_dev *parent;
    u64 lba_start;
    u64 lba_count;
};

static EFI_STATUS EFIAPI blk_reset(EFI_BLOCK_IO_PROTOCOL *this, BOOLEAN extended)
{
    (void)this;
    (void)extended;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI blk_flush(EFI_BLOCK_IO_PROTOCOL *this)
{
    (void)this;
    return EFI_SUCCESS;
}

static EFI_STATUS disk_rw(struct blk_dev *d, u64 lba, UINTN bytes, void *buf, int write)
{
    if (!d->media.media_present)
        return EFI_NO_MEDIA;
    if (!bytes)
        return EFI_SUCCESS;
    if (bytes % d->media.block_size)
        return EFI_BAD_BUFFER_SIZE;
    u64 blocks = bytes / d->media.block_size;
    if (lba > d->media.last_block || blocks - 1 > d->media.last_block - lba)
        return EFI_INVALID_PARAMETER;
    /* lba_start is absolute on the driver's disk for every node. */
    u64 abs = d->lba_start + lba;
    u8 *p = buf;
    while (bytes) {                     /* drivers take up to 1 GB a call */
        u32 n = bytes > (1u << 30) ? 1u << 30 : (u32)bytes;
        int ok = write ? d->dev->write(d->dev, abs, p, n) : d->dev->read(d->dev, abs, p, n);
        if (!ok)
            return EFI_DEVICE_ERROR;
        abs += n / 512;
        p += n;
        bytes -= n;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI blk_read(EFI_BLOCK_IO_PROTOCOL *this, u64 media_id, u64 lba,
                                  UINTN bytes, void *buf)
{
    struct blk_dev *d = (struct blk_dev *)this;
    if (!d || !buf)
        return EFI_INVALID_PARAMETER;
    if (media_id != d->media.media_id)
        return EFI_MEDIA_CHANGED;
    return disk_rw(d, lba, bytes, buf, 0);
}

static EFI_STATUS EFIAPI blk_write(EFI_BLOCK_IO_PROTOCOL *this, u64 media_id, u64 lba,
                                   UINTN bytes, void *buf)
{
    struct blk_dev *d = (struct blk_dev *)this;
    if (!d || !buf)
        return EFI_INVALID_PARAMETER;
    if (media_id != d->media.media_id)
        return EFI_MEDIA_CHANGED;
    if (d->media.read_only)
        return EFI_WRITE_PROTECTED;
    return disk_rw(d, lba, bytes, buf, 1);
}

static void blk_fill(struct blk_dev *d, u64 last, int partition)
{
    d->bio.revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    d->bio.media = &d->media;
    d->bio.reset = blk_reset;
    d->bio.read_blocks = blk_read;
    d->bio.write_blocks = blk_write;
    d->bio.flush_blocks = blk_flush;
    d->media.media_id = 1;
    d->media.removable_media = 0;
    d->media.media_present = 1;
    d->media.logical_partition = partition ? 1 : 0;
    d->media.read_only = 0;
    d->media.write_caching = 0;
    d->media.block_size = 512;
    d->media.io_align = 4;
    d->media.last_block = last;
}

static struct blk_dev *blk_new(u64 lba_start, u64 lba_count, struct blk_dev *parent,
                               EFI_DEVICE_PATH_PROTOCOL *path, struct sq_blk *dev)
{
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct blk_dev), &mem) != EFI_SUCCESS)
        return NULL;
    struct blk_dev *d = mem;
    memset(d, 0, sizeof *d);
    d->parent = parent;
    d->dev = parent ? parent->dev : dev;
    d->lba_start = lba_start;
    d->lba_count = lba_count;
    d->path = path;
    blk_fill(d, lba_count ? lba_count - 1 : 0, parent != NULL);
    EFI_HANDLE h = NULL;
    if (efi_install_protocol_interface(&h, (EFI_GUID *)&efi_block_io_guid, 0, &d->bio) != EFI_SUCCESS) {
        efi_free_pool(d);
        return NULL;
    }
    if (path)
        efi_install_protocol_interface(&h, (EFI_GUID *)&efi_device_path_guid, 0, path);
    d->handle = h;
    return d;
}

static int guid_match(const u8 *a, const u8 *b)
{
    return memcmp(a, b, 16) == 0;
}

static void try_fat(struct blk_dev *d)
{
    fat_install(d->handle, &d->bio);
}

static void scan_gpt(struct blk_dev *disk)
{
    u8 hdr[512];
    if (disk->bio.read_blocks(&disk->bio, 1, 1, 512, hdr) != EFI_SUCCESS)
        return;
    if (memcmp(hdr, "EFI PART", 8))
        return;
    u64 ent_lba = 0;
    u32 ent_size = 0, ent_count = 0;
    memcpy(&ent_lba, hdr + 72, 8);
    memcpy(&ent_count, hdr + 80, 4);
    memcpy(&ent_size, hdr + 84, 4);
    if (ent_size < 128 || ent_count == 0 || ent_count > 128)
        return;

    u32 bytes = ent_count * ent_size;
    u32 secs = (bytes + 511) / 512;
    void *ents;
    if (efi_allocate_pool(EfiBootServicesData, secs * 512, &ents) != EFI_SUCCESS)
        return;
    if (disk->bio.read_blocks(&disk->bio, 1, ent_lba, secs * 512, ents) != EFI_SUCCESS) {
        efi_free_pool(ents);
        return;
    }
    printf("disk: GPT, %u partitions\n", ent_count);
    for (u32 i = 0; i < ent_count; i++) {
        u8 *e = (u8 *)ents + i * ent_size;
        u8 zero[16] = {0};
        if (guid_match(e, zero))
            continue;
        u64 start, end;
        memcpy(&start, e + 32, 8);
        memcpy(&end, e + 40, 8);
        if (end < start)
            continue;
        u64 count = end - start + 1;
        int is_esp = guid_match(e, esp_guid);
        EFI_DEVICE_PATH_PROTOCOL *hd = efi_dp_hd(i + 1, start, count, e + 16);
        EFI_DEVICE_PATH_PROTOCOL *path = efi_dp_append(disk->path, hd);
        if (hd)
            efi_free_pool(hd);
        struct blk_dev *p = blk_new(start, count, disk, path, NULL);
        if (!p)
            continue;
        printf("disk: partition %u LBA %lu..%lu%s\n", i + 1, start, end,
               is_esp ? " (ESP)" : "");
        try_fat(p);
    }
    efi_free_pool(ents);
}

static void scan_mbr(struct blk_dev *disk)
{
    u8 mbr[512];
    if (disk->bio.read_blocks(&disk->bio, 1, 0, 512, mbr) != EFI_SUCCESS)
        return;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return;
    /* Protective GPT MBR: one 0xEE partition — leave it to scan_gpt. */
    int ee = 0, any = 0;
    for (int i = 0; i < 4; i++) {
        u8 *p = mbr + 446 + i * 16;
        if (!p[4])
            continue;
        any = 1;
        if (p[4] == 0xEE)
            ee = 1;
    }
    if (ee || !any)
        return;
    for (int i = 0; i < 4; i++) {
        u8 *p = mbr + 446 + i * 16;
        u8 type = p[4];
        if (!type)
            continue;
        u32 start = (u32)p[8] | (u32)p[9] << 8 | (u32)p[10] << 16 | (u32)p[11] << 24;
        u32 count = (u32)p[12] | (u32)p[13] << 8 | (u32)p[14] << 16 | (u32)p[15] << 24;
        if (!count)
            continue;
        u8 uuid[16] = {0};
        uuid[0] = (u8)(i + 1);
        EFI_DEVICE_PATH_PROTOCOL *hd = efi_dp_hd(i + 1, start, count, uuid);
        EFI_DEVICE_PATH_PROTOCOL *path = efi_dp_append(disk->path, hd);
        if (hd)
            efi_free_pool(hd);
        struct blk_dev *part = blk_new(start, count, disk, path, NULL);
        if (!part)
            continue;
        printf("disk: MBR partition %d type %02x LBA %u+%u\n", i + 1, type, start, count);
        try_fat(part);
    }
}

/* A driver found a disk: publish it, its partitions and file systems */
void efi_disk_add(struct sq_blk *dev)
{
    struct blk_dev *disk = blk_new(0, dev->sectors, NULL, dev->path, dev);
    if (!disk)
        return;
    /* Prefer GPT; also try whole-disk FAT (removable-style) and MBR. */
    scan_gpt(disk);
    scan_mbr(disk);
    try_fat(disk);
}

/* Every disk driver looks for its devices and calls efi_disk_add */
void efi_disk_init(void)
{
    if (!arch_disk_probe())
        printf("disk: no disks\n");
}

/* The boot file name as ASCII, for messages */
static const char *boot_name(void)
{
    static char name[40];
    const CHAR16 *p = ARCH_BOOT_FILE;
    usize i = 0;
    for (; p[i] && i < sizeof name - 1; i++)
        name[i] = (char)p[i];
    name[i] = 0;
    return name;
}

/* Boot \EFI\BOOT\BOOT<arch>.EFI from the first Simple File System that has it. */
int efi_boot_esp(void)
{
    static CHAR16 path[] = ARCH_BOOT_FILE;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS s = ((EFI_STATUS (EFIAPI *)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *,
                                           UINTN *, EFI_HANDLE **))
                    efi_st->boot_services->locate_handle_buffer)(
        ByProtocol, (EFI_GUID *)&efi_simple_file_system_guid, NULL, &count, &handles);
    if (s != EFI_SUCCESS || !count)
        return 0;

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
        if (efi_handle_protocol(handles[i], (EFI_GUID *)&efi_simple_file_system_guid,
                                (void **)&sfs) != EFI_SUCCESS)
            continue;
        EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
        if (sfs->open_volume(sfs, &root) != EFI_SUCCESS)
            continue;
        if (root->open(root, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
            root->close(root);
            continue;
        }
        /* Size via GetInfo, then read the whole file. */
        UINTN info_sz = 0;
        file->get_info(file, (EFI_GUID *)&efi_file_info_guid, &info_sz, NULL);
        void *infomem;
        if (efi_allocate_pool(EfiBootServicesData, info_sz ? info_sz : 256, &infomem) != EFI_SUCCESS) {
            file->close(file);
            root->close(root);
            continue;
        }
        info_sz = info_sz ? info_sz : 256;
        if (file->get_info(file, (EFI_GUID *)&efi_file_info_guid, &info_sz, infomem) != EFI_SUCCESS) {
            efi_free_pool(infomem);
            file->close(file);
            root->close(root);
            continue;
        }
        EFI_FILE_INFO *info = infomem;
        UINTN fsize = (UINTN)info->file_size;
        efi_free_pool(infomem);
        if (!fsize || fsize > 256 * 1024 * 1024) {
            file->close(file);
            root->close(root);
            continue;
        }
        void *buf;
        if (efi_allocate_pool(EfiBootServicesData, fsize, &buf) != EFI_SUCCESS) {
            file->close(file);
            root->close(root);
            continue;
        }
        UINTN n = fsize;
        if (file->read(file, &n, buf) != EFI_SUCCESS || n != fsize) {
            efi_free_pool(buf);
            file->close(file);
            root->close(root);
            continue;
        }
        file->close(file);
        root->close(root);

        printf("efi: booting %s (%lu bytes)\n", boot_name(), (u64)fsize);
        EFI_HANDLE img;
        s = efi_load_image(0, efi_firmware_handle, NULL, buf, fsize, &img);
        efi_free_pool(buf);
        if (s != EFI_SUCCESS) {
            printf("efi: LoadImage failed: 0x%lx\n", s);
            efi_free_pool(handles);
            return 0;
        }
        /* Remember which device the image came from. */
        EFI_LOADED_IMAGE_PROTOCOL *li;
        if (efi_handle_protocol(img, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li) == EFI_SUCCESS) {
            li->device_handle = handles[i];
            li->file_path = efi_dp_file(path);
        }
        arch_boot_options(img);
        UINTN xsz = 0;
        CHAR16 *xdata = NULL;
        s = efi_start_image(img, &xsz, &xdata);
        printf("efi: %s returned 0x%lx\n", boot_name() + 10, s);
        efi_free_pool(handles);
        return 1;
    }
    efi_free_pool(handles);
    return 0;
}
