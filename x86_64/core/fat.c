/* SquirrelBIOS x86_64-uefi: FAT12/16/32 Simple File System (read-only).
 * Original code, from the Microsoft FAT specification. Opens volumes on
 * a Block I/O handle; path walks use 8.3 names (enough for \EFI\BOOT\
 * BOOTX64.EFI). Long-name entries are skipped. */
#include "efi.h"

const EFI_GUID efi_simple_file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
const EFI_GUID efi_file_info_guid = EFI_FILE_INFO_GUID;

#define FAT_MAGIC 0x7461465173717253ULL         /* "SqFsFat" */
#define FILE_MAGIC 0x656c694651737253ULL        /* "SqFile" */

enum { FAT12 = 12, FAT16 = 16, FAT32 = 32 };

struct fat_vol {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;        /* first */
    u64 magic;
    EFI_HANDLE handle;
    EFI_BLOCK_IO_PROTOCOL *bio;
    u32 media_id;
    u32 bytes_per_sec;
    u32 sec_per_clus;
    u32 reserved;
    u32 fats;
    u32 root_entries;                           /* 0 on FAT32 */
    u32 fat_secs;
    u32 total_secs;
    u32 root_clus;                              /* FAT32 */
    u32 data_start;                             /* LBA of cluster 2 */
    u32 root_secs;
    int kind;
    u8 *secbuf;
};

struct fat_file {
    EFI_FILE_PROTOCOL file;                     /* first */
    u64 magic;
    struct fat_vol *vol;
    u32 start_clus;
    u32 size;
    u64 pos;
    u32 attr;
    int is_root;
    CHAR16 name[13];
};

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }

static EFI_STATUS bio_read(struct fat_vol *v, u64 lba, void *buf, UINTN bytes)
{
    return v->bio->read_blocks(v->bio, v->media_id, lba, bytes, buf);
}

static EFI_STATUS clus_to_lba(struct fat_vol *v, u32 clus, u64 *lba)
{
    if (clus < 2)
        return EFI_INVALID_PARAMETER;
    *lba = v->data_start + (u64)(clus - 2) * v->sec_per_clus;
    return EFI_SUCCESS;
}

static EFI_STATUS fat_next(struct fat_vol *v, u32 clus, u32 *out)
{
    u32 off, ent;
    u64 lba;
    if (v->kind == FAT32) {
        off = clus * 4;
        lba = v->reserved + off / v->bytes_per_sec;
        if (bio_read(v, lba, v->secbuf, v->bytes_per_sec) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        ent = rd32(v->secbuf + (off % v->bytes_per_sec)) & 0x0FFFFFFF;
        if (ent >= 0x0FFFFFF8)
            ent = 0xFFFFFFFF;
    } else if (v->kind == FAT16) {
        off = clus * 2;
        lba = v->reserved + off / v->bytes_per_sec;
        if (bio_read(v, lba, v->secbuf, v->bytes_per_sec) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        ent = rd16(v->secbuf + (off % v->bytes_per_sec));
        if (ent >= 0xFFF8)
            ent = 0xFFFFFFFF;
    } else {
        off = clus + clus / 2;
        lba = v->reserved + off / v->bytes_per_sec;
        if (bio_read(v, lba, v->secbuf, v->bytes_per_sec) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        u16 w = rd16(v->secbuf + (off % v->bytes_per_sec));
        ent = (clus & 1) ? (w >> 4) : (w & 0x0FFF);
        if (ent >= 0x0FF8)
            ent = 0xFFFFFFFF;
    }
    *out = ent;
    return EFI_SUCCESS;
}

static int name83_eq(const u8 *e, const char *name8, const char *ext3)
{
    for (int i = 0; i < 8; i++) {
        char c = name8[i] ? name8[i] : ' ';
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (e[i] != (u8)c)
            return 0;
    }
    for (int i = 0; i < 3; i++) {
        char c = ext3[i] ? ext3[i] : ' ';
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (e[8 + i] != (u8)c)
            return 0;
    }
    return 1;
}

/* Split a UCS-2 path component into 8.3 ASCII (uppercase). */
static int split83(const CHAR16 *n, char name8[9], char ext3[4])
{
    memset(name8, 0, 9);
    memset(ext3, 0, 4);
    int i = 0, j = 0;
    while (n[i] && n[i] != '.' && j < 8) {
        CHAR16 c = n[i++];
        if (c > 0x7F)
            return 0;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        name8[j++] = (char)c;
    }
    if (n[i] == '.') {
        i++;
        j = 0;
        while (n[i] && j < 3) {
            CHAR16 c = n[i++];
            if (c > 0x7F)
                return 0;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            ext3[j++] = (char)c;
        }
    }
    return n[i] == 0;
}

static EFI_STATUS read_clus(struct fat_vol *v, u32 clus, u8 *buf)
{
    u64 lba;
    EFI_STATUS s = clus_to_lba(v, clus, &lba);
    if (s != EFI_SUCCESS)
        return s;
    return bio_read(v, lba, buf, (UINTN)v->sec_per_clus * v->bytes_per_sec);
}

static EFI_STATUS dir_find(struct fat_vol *v, u32 dir_clus, int is_root,
                           const CHAR16 *comp, u32 *out_clus, u32 *out_size, u32 *out_attr)
{
    char name8[9], ext3[4];
    if (!split83(comp, name8, ext3))
        return EFI_NOT_FOUND;

    u32 clus_bytes = v->sec_per_clus * v->bytes_per_sec;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, clus_bytes > 4096 ? clus_bytes : 4096, &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    u8 *buf = mem;
    EFI_STATUS s = EFI_NOT_FOUND;

    if (is_root && v->kind != FAT32) {
        u32 left = v->root_entries * 32;
        u64 lba = v->reserved + v->fats * v->fat_secs;
        while (left) {
            UINTN n = left > v->bytes_per_sec ? v->bytes_per_sec : left;
            if (bio_read(v, lba++, buf, v->bytes_per_sec) != EFI_SUCCESS) {
                s = EFI_DEVICE_ERROR;
                goto out;
            }
            for (UINTN off = 0; off + 32 <= n; off += 32) {
                u8 *e = buf + off;
                if (e[0] == 0)
                    goto out;
                if (e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (name83_eq(e, name8, ext3)) {
                    *out_clus = rd16(e + 26) | ((u32)rd16(e + 20) << 16);
                    *out_size = rd32(e + 28);
                    *out_attr = e[11];
                    s = EFI_SUCCESS;
                    goto out;
                }
            }
            left -= n;
        }
        goto out;
    }

    u32 clus = is_root ? v->root_clus : dir_clus;
    while (clus && clus != 0xFFFFFFFF) {
        if (read_clus(v, clus, buf) != EFI_SUCCESS) {
            s = EFI_DEVICE_ERROR;
            goto out;
        }
        for (u32 off = 0; off + 32 <= clus_bytes; off += 32) {
            u8 *e = buf + off;
            if (e[0] == 0)
                goto out;
            if (e[0] == 0xE5 || e[11] == 0x0F)
                continue;
            if (name83_eq(e, name8, ext3)) {
                *out_clus = rd16(e + 26) | ((u32)rd16(e + 20) << 16);
                *out_size = rd32(e + 28);
                *out_attr = e[11];
                s = EFI_SUCCESS;
                goto out;
            }
        }
        if (fat_next(v, clus, &clus) != EFI_SUCCESS) {
            s = EFI_DEVICE_ERROR;
            goto out;
        }
    }
out:
    efi_free_pool(mem);
    return s;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *this)
{
    struct fat_file *f = (struct fat_file *)this;
    if (!f || f->magic != FILE_MAGIC)
        return EFI_INVALID_PARAMETER;
    f->magic = 0;
    return efi_free_pool(f);
}

static EFI_STATUS EFIAPI file_delete(EFI_FILE_PROTOCOL *this)
{
    (void)this;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *this, UINTN *size, void *buf)
{
    (void)this;
    (void)size;
    (void)buf;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *this)
{
    (void)this;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_info(EFI_FILE_PROTOCOL *this, EFI_GUID *g, UINTN n, void *b)
{
    (void)this;
    (void)g;
    (void)n;
    (void)b;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_get_position(EFI_FILE_PROTOCOL *this, u64 *pos)
{
    struct fat_file *f = (struct fat_file *)this;
    if (!f || f->magic != FILE_MAGIC || !pos)
        return EFI_INVALID_PARAMETER;
    *pos = f->pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(EFI_FILE_PROTOCOL *this, u64 pos)
{
    struct fat_file *f = (struct fat_file *)this;
    if (!f || f->magic != FILE_MAGIC)
        return EFI_INVALID_PARAMETER;
    if (pos == ~0ULL)
        pos = f->size;
    if (!(f->attr & EFI_FILE_DIRECTORY) && pos > f->size)
        return EFI_UNSUPPORTED;
    f->pos = pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(EFI_FILE_PROTOCOL *this, EFI_GUID *guid, UINTN *size, void *buf)
{
    struct fat_file *f = (struct fat_file *)this;
    if (!f || f->magic != FILE_MAGIC || !guid || !size)
        return EFI_INVALID_PARAMETER;
    if (!guid_eq(guid, &efi_file_info_guid))
        return EFI_UNSUPPORTED;
    UINTN need = sizeof(EFI_FILE_INFO) + 12 * sizeof(CHAR16);
    if (*size < need || !buf) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    EFI_FILE_INFO *info = buf;
    memset(info, 0, need);
    info->size = need;
    info->file_size = f->size;
    info->physical_size = f->size;
    info->attribute = f->attr;
    for (int i = 0; f->name[i]; i++)
        info->file_name[i] = f->name[i];
    *size = need;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *this, UINTN *size, void *buf)
{
    struct fat_file *f = (struct fat_file *)this;
    if (!f || f->magic != FILE_MAGIC || !size || !buf)
        return EFI_INVALID_PARAMETER;
    if (f->attr & EFI_FILE_DIRECTORY)
        return EFI_UNSUPPORTED;                 /* directory iteration later */
    if (f->pos >= f->size) {
        *size = 0;
        return EFI_SUCCESS;
    }
    UINTN want = *size;
    if (f->pos + want > f->size)
        want = (UINTN)(f->size - f->pos);

    struct fat_vol *v = f->vol;
    u32 clus_bytes = v->sec_per_clus * v->bytes_per_sec;
    void *cbuf;
    if (efi_allocate_pool(EfiBootServicesData, clus_bytes, &cbuf) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;

    u32 clus = f->start_clus;
    u64 skip = f->pos;
    while (skip >= clus_bytes) {
        if (fat_next(v, clus, &clus) != EFI_SUCCESS || clus == 0xFFFFFFFF) {
            efi_free_pool(cbuf);
            return EFI_DEVICE_ERROR;
        }
        skip -= clus_bytes;
    }

    u8 *dst = buf;
    UINTN left = want;
    while (left) {
        if (read_clus(v, clus, cbuf) != EFI_SUCCESS) {
            efi_free_pool(cbuf);
            return EFI_DEVICE_ERROR;
        }
        UINTN take = clus_bytes - (UINTN)skip;
        if (take > left)
            take = left;
        memcpy(dst, (u8 *)cbuf + skip, take);
        dst += take;
        left -= take;
        skip = 0;
        if (left) {
            if (fat_next(v, clus, &clus) != EFI_SUCCESS || clus == 0xFFFFFFFF) {
                efi_free_pool(cbuf);
                return EFI_DEVICE_ERROR;
            }
        }
    }
    efi_free_pool(cbuf);
    f->pos += want;
    *size = want;
    return EFI_SUCCESS;
}

static void fill_file_ops(EFI_FILE_PROTOCOL *f);

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *this, EFI_FILE_PROTOCOL **newh,
                                   CHAR16 *name, u64 mode, u64 attr)
{
    struct fat_file *parent = (struct fat_file *)this;
    (void)attr;
    if (!parent || parent->magic != FILE_MAGIC || !newh || !name)
        return EFI_INVALID_PARAMETER;
    if (mode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
        return EFI_UNSUPPORTED;
    if (!(mode & EFI_FILE_MODE_READ))
        return EFI_INVALID_PARAMETER;

    /* Absolute from root, or relative. Skip leading backslashes. */
    CHAR16 *p = name;
    int from_root = 0;
    while (*p == '\\' || *p == '/') {
        from_root = 1;
        p++;
    }
    u32 clus = from_root ? 0 : parent->start_clus;
    int is_root = from_root || parent->is_root;
    u32 size = 0, fat_attr = EFI_FILE_DIRECTORY;

    if (!*p) {
        /* Open the volume root again */
        void *mem;
        if (efi_allocate_pool(EfiBootServicesData, sizeof(struct fat_file), &mem) != EFI_SUCCESS)
            return EFI_OUT_OF_RESOURCES;
        struct fat_file *f = mem;
        memset(f, 0, sizeof *f);
        f->magic = FILE_MAGIC;
        f->vol = parent->vol;
        f->is_root = 1;
        f->start_clus = parent->vol->kind == FAT32 ? parent->vol->root_clus : 0;
        f->attr = EFI_FILE_DIRECTORY;
        f->name[0] = '\\';
        fill_file_ops(&f->file);
        *newh = &f->file;
        return EFI_SUCCESS;
    }

    while (*p) {
        CHAR16 comp[14];
        int i = 0;
        while (*p && *p != '\\' && *p != '/' && i < 13)
            comp[i++] = *p++;
        comp[i] = 0;
        while (*p == '\\' || *p == '/')
            p++;
        if (!i)
            break;
        u32 next, nsize, nattr;
        EFI_STATUS s = dir_find(parent->vol, clus, is_root, comp, &next, &nsize, &nattr);
        if (s != EFI_SUCCESS)
            return s;
        clus = next;
        size = nsize;
        fat_attr = nattr;
        is_root = 0;
        if (*p && !(nattr & 0x10))
            return EFI_NOT_FOUND;
    }

    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct fat_file), &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    struct fat_file *f = mem;
    memset(f, 0, sizeof *f);
    f->magic = FILE_MAGIC;
    f->vol = parent->vol;
    f->start_clus = clus;
    f->size = size;
    f->attr = (fat_attr & 0x10) ? EFI_FILE_DIRECTORY : EFI_FILE_READ_ONLY;
    fill_file_ops(&f->file);
    *newh = &f->file;
    return EFI_SUCCESS;
}

static void fill_file_ops(EFI_FILE_PROTOCOL *f)
{
    f->revision = EFI_FILE_PROTOCOL_REVISION;
    f->open = file_open;
    f->close = file_close;
    f->delete = file_delete;
    f->read = file_read;
    f->write = file_write;
    f->get_position = file_get_position;
    f->set_position = file_set_position;
    f->get_info = file_get_info;
    f->set_info = file_set_info;
    f->flush = file_flush;
}

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *this, EFI_FILE_PROTOCOL **root)
{
    struct fat_vol *v = (struct fat_vol *)this;
    if (!v || v->magic != FAT_MAGIC || !root)
        return EFI_INVALID_PARAMETER;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct fat_file), &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    struct fat_file *f = mem;
    memset(f, 0, sizeof *f);
    f->magic = FILE_MAGIC;
    f->vol = v;
    f->is_root = 1;
    f->start_clus = v->kind == FAT32 ? v->root_clus : 0;
    f->attr = EFI_FILE_DIRECTORY;
    f->name[0] = '\\';
    fill_file_ops(&f->file);
    *root = &f->file;
    return EFI_SUCCESS;
}

/* Probe a Block I/O media for a FAT volume; on success install SFS on handle. */
int fat_install(EFI_HANDLE handle, EFI_BLOCK_IO_PROTOCOL *bio)
{
    if (!bio || !bio->media || !bio->media->media_present)
        return 0;
    u32 bsz = bio->media->block_size;
    if (bsz < 512 || bsz > 4096)
        return 0;
    void *sec;
    if (efi_allocate_pool(EfiBootServicesData, bsz, &sec) != EFI_SUCCESS)
        return 0;
    if (bio->read_blocks(bio, bio->media->media_id, 0, bsz, sec) != EFI_SUCCESS) {
        efi_free_pool(sec);
        return 0;
    }
    u8 *b = sec;
    if (b[0] != 0xEB && b[0] != 0xE9) {
        efi_free_pool(sec);
        return 0;
    }
    if (b[510] != 0x55 || b[511] != 0xAA) {
        efi_free_pool(sec);
        return 0;
    }
    u16 bps = rd16(b + 11);
    u8 spc = b[13];
    u16 reserved = rd16(b + 14);
    u8 fats = b[16];
    u16 root_ent = rd16(b + 17);
    u16 tot16 = rd16(b + 19);
    u16 fat16 = rd16(b + 22);
    u32 tot32 = rd32(b + 32);
    u32 fat32 = rd32(b + 36);
    u32 root_clus = rd32(b + 44);
    if (bps == 0 || spc == 0 || fats == 0 || reserved == 0) {
        efi_free_pool(sec);
        return 0;
    }

    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct fat_vol), &mem) != EFI_SUCCESS) {
        efi_free_pool(sec);
        return 0;
    }
    struct fat_vol *v = mem;
    memset(v, 0, sizeof *v);
    v->magic = FAT_MAGIC;
    v->handle = handle;
    v->bio = bio;
    v->media_id = bio->media->media_id;
    v->bytes_per_sec = bps;
    v->sec_per_clus = spc;
    v->reserved = reserved;
    v->fats = fats;
    v->root_entries = root_ent;
    v->total_secs = tot16 ? tot16 : tot32;
    v->secbuf = sec;

    if (fat16 == 0) {
        v->kind = FAT32;
        v->fat_secs = fat32;
        v->root_clus = root_clus ? root_clus : 2;
        v->root_secs = 0;
    } else {
        v->fat_secs = fat16;
        v->root_secs = (root_ent * 32 + bps - 1) / bps;
        u32 data_secs = v->total_secs - reserved - fats * fat16 - v->root_secs;
        u32 clusters = data_secs / spc;
        v->kind = clusters < 4085 ? FAT12 : FAT16;
        v->root_clus = 0;
    }
    v->data_start = reserved + fats * v->fat_secs + v->root_secs;
    v->sfs.revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    v->sfs.open_volume = open_volume;

    if (efi_install_protocol_interface(&handle, (EFI_GUID *)&efi_simple_file_system_guid,
                                       0, &v->sfs) != EFI_SUCCESS) {
        efi_free_pool(sec);
        efi_free_pool(v);
        return 0;
    }
    printf("fs: FAT%d on %s, %u KB\n", v->kind,
           bio->media->logical_partition ? "partition" : "disk",
           (u32)((v->total_secs * (u64)bps) >> 10));
    return 1;
}
