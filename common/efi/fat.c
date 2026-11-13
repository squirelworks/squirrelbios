/* SquirrelBIOS UEFI core (common/efi): FAT12/16/32 Simple File System.
 * Original code, from the Microsoft FAT specification (BPB, FAT entries,
 * directory entries, long names and their checksum, FSInfo) and the UEFI
 * specification (EFI_FILE_PROTOCOL). Started as a copy of
 * x86_64/core/fat.c.
 *
 * Opens volumes on a Block I/O handle. A path component matches an
 * entry's long name or its 8.3 name, case ignored (ASCII). Files can be
 * read, written (growing as needed), truncated or extended (SetInfo),
 * renamed within their directory and deleted; directories can be created
 * and listed (Read returns EFI_FILE_INFO). Writes go straight to the
 * disk, every FAT copy updated. New names that are not plain upper-case
 * 8.3 get long-name entries and a NAME~N alias. */
#include "efi.h"

const EFI_GUID efi_simple_file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
const EFI_GUID efi_file_info_guid = EFI_FILE_INFO_GUID;

#define FAT_MAGIC 0x7461465173717253ULL         /* "SqFsFat" */
#define FILE_MAGIC 0x656c694651737253ULL        /* "SqFile" */

#define EFI_VOLUME_FULL          EFI_ERR(11)
#define EFI_WARN_DELETE_FAILURE  2

#define EOC       0xFFFFFFFFu                   /* end of chain, any FAT */
#define LFN_MAX   255

#define A_RO      0x01
#define A_DIR     0x10
#define A_ARCHIVE 0x20
#define A_LFN     0x0F
#define A_LABEL   0x08
#define A_SETTABLE (0x01 | 0x02 | 0x04 | 0x20)  /* read-only, hidden, system, archive */

enum { FAT12 = 12, FAT16 = 16, FAT32 = 32 };

struct fat_vol {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;        /* first */
    u64 magic;
    EFI_HANDLE handle;
    EFI_BLOCK_IO_PROTOCOL *bio;
    u32 media_id;
    u32 bps;                                    /* bytes per sector */
    u32 spc;                                    /* sectors per cluster */
    u32 clus_bytes;
    u32 reserved;
    u32 fats;
    u32 root_entries;                           /* 0 on FAT32 */
    u32 fat_secs;
    u32 total_secs;
    u32 root_clus;                              /* FAT32 */
    u32 data_start;                             /* LBA of cluster 2 */
    u32 root_secs;
    u32 max_clus;                               /* the last data cluster */
    u32 next_free;                              /* where the free search starts */
    u32 fsinfo;                                 /* FAT32 FSInfo sector, or 0 */
    int fsinfo_stale;                           /* its free count invalidated */
    int kind;
    int read_only;
    u8 *fatbuf;                                 /* two sectors (FAT12 entries straddle) */
    u64 fat_lba;                                /* what fatbuf holds, or ~0 */
    u8 *dirbuf;                                 /* one sector of a directory */
    u64 dir_lba;
};

struct fat_file {
    EFI_FILE_PROTOCOL file;                     /* first */
    u64 magic;
    struct fat_vol *vol;
    u32 start_clus;                             /* 0: none yet (or the FAT12/16 root) */
    u32 size;
    u64 pos;                                    /* bytes; a directory's next slot */
    u8 attr;                                    /* FAT attribute bits */
    int is_root;
    int writable;
    /* Where its entry is: the parent directory and the slots of its
     * long name (first) and 8.3 entry (last) */
    u32 dir_clus;
    int dir_is_root;
    u32 first_slot, slot;
    CHAR16 name[LFN_MAX + 1];
};

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }

static CHAR16 upcase(CHAR16 c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static UINTN wcslen16(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n])
        n++;
    return n;
}

/* --- sectors --------------------------------------------------------- */

static EFI_STATUS bio_read(struct fat_vol *v, u64 lba, void *buf, UINTN bytes)
{
    return v->bio->read_blocks(v->bio, v->media_id, lba, bytes, buf);
}

static EFI_STATUS bio_write(struct fat_vol *v, u64 lba, void *buf, UINTN bytes)
{
    if (v->read_only)
        return EFI_WRITE_PROTECTED;
    if (lba <= v->dir_lba && v->dir_lba < lba + bytes / v->bps)
        v->dir_lba = ~0ULL;                     /* the caches go stale */
    if (lba < v->fat_lba + 2 && v->fat_lba < lba + bytes / v->bps)
        v->fat_lba = ~0ULL;
    return v->bio->write_blocks(v->bio, v->media_id, lba, bytes, buf);
}

static u64 clus_lba(struct fat_vol *v, u32 clus)
{
    return v->data_start + (u64)(clus - 2) * v->spc;
}

static EFI_STATUS read_clus(struct fat_vol *v, u32 clus, void *buf)
{
    if (clus < 2 || clus > v->max_clus)
        return EFI_VOLUME_CORRUPTED;
    return bio_read(v, clus_lba(v, clus), buf, v->clus_bytes);
}

static EFI_STATUS write_clus(struct fat_vol *v, u32 clus, void *buf)
{
    if (clus < 2 || clus > v->max_clus)
        return EFI_VOLUME_CORRUPTED;
    return bio_write(v, clus_lba(v, clus), buf, v->clus_bytes);
}

/* --- the FAT --------------------------------------------------------- */

/* The FAT sector(s) holding clus's entry in copy n: its LBA, the offset */
static void fat_where(struct fat_vol *v, u32 n, u32 clus, u64 *lba, u32 *off)
{
    u32 byte = v->kind == FAT32 ? clus * 4 : v->kind == FAT16 ? clus * 2 : clus + clus / 2;
    *lba = v->reserved + (u64)n * v->fat_secs + byte / v->bps;
    *off = byte % v->bps;
}

static EFI_STATUS fat_load(struct fat_vol *v, u64 lba)
{
    if (v->fat_lba == lba)
        return EFI_SUCCESS;
    u32 n = v->kind == FAT12 ? 2 : 1;           /* a FAT12 entry may straddle */
    EFI_STATUS s = bio_read(v, lba, v->fatbuf, (UINTN)n * v->bps);
    v->fat_lba = s == EFI_SUCCESS ? lba : ~0ULL;
    return s;
}

static EFI_STATUS fat_get(struct fat_vol *v, u32 clus, u32 *out)
{
    u64 lba;
    u32 off;
    fat_where(v, 0, clus, &lba, &off);
    if (fat_load(v, lba) != EFI_SUCCESS)
        return EFI_DEVICE_ERROR;
    u8 *p = v->fatbuf + off;
    u32 e;
    if (v->kind == FAT32) {
        e = rd32(p) & 0x0FFFFFFF;
        if (e >= 0x0FFFFFF8)
            e = EOC;
    } else if (v->kind == FAT16) {
        e = rd16(p);
        if (e >= 0xFFF8)
            e = EOC;
    } else {
        u16 w = rd16(p);
        e = (clus & 1) ? w >> 4 : w & 0x0FFF;
        if (e >= 0x0FF8)
            e = EOC;
    }
    *out = e;
    return EFI_SUCCESS;
}

static EFI_STATUS fat_set(struct fat_vol *v, u32 clus, u32 val)
{
    for (u32 n = 0; n < v->fats; n++) {
        u64 lba;
        u32 off;
        fat_where(v, n, clus, &lba, &off);
        if (fat_load(v, lba) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        u8 *p = v->fatbuf + off;
        if (v->kind == FAT32) {
            u32 e = val == EOC ? 0x0FFFFFFF : val;
            wr32(p, (rd32(p) & 0xF0000000) | (e & 0x0FFFFFFF));
        } else if (v->kind == FAT16) {
            wr16(p, (u16)(val == EOC ? 0xFFFF : val));
        } else {
            u16 e = (u16)(val == EOC ? 0x0FFF : val & 0x0FFF);
            u16 w = rd16(p);
            w = (clus & 1) ? (u16)((w & 0x000F) | e << 4) : (u16)((w & 0xF000) | e);
            wr16(p, w);
        }
        u32 secs = (v->kind == FAT12 && off + 1 >= v->bps) ? 2 : 1;
        EFI_STATUS s = bio_write(v, lba, v->fatbuf, (UINTN)secs * v->bps);
        if (s != EFI_SUCCESS)
            return s;
        v->fat_lba = ~0ULL;
    }
    return EFI_SUCCESS;
}

/* FAT32: the free-cluster count in FSInfo is only a hint; once we change
 * the FAT, say it is unknown */
static void fsinfo_invalidate(struct fat_vol *v)
{
    if (!v->fsinfo || v->fsinfo_stale)
        return;
    v->fsinfo_stale = 1;
    if (bio_read(v, v->fsinfo, v->dirbuf, v->bps) != EFI_SUCCESS)
        return;
    v->dir_lba = ~0ULL;
    if (rd32(v->dirbuf) == 0x41615252 && rd32(v->dirbuf + 484) == 0x61417272) {
        wr32(v->dirbuf + 488, 0xFFFFFFFF);
        bio_write(v, v->fsinfo, v->dirbuf, v->bps);
    }
}

/* A free cluster, zeroed, marked the end of a chain and linked after
 * prev (0: a new chain) */
static EFI_STATUS clus_alloc(struct fat_vol *v, u32 prev, u8 *zero, u32 *out)
{
    u32 count = v->max_clus - 1;
    u32 c = v->next_free < 2 || v->next_free > v->max_clus ? 2 : v->next_free;
    for (u32 i = 0; i < count; i++, c = c == v->max_clus ? 2 : c + 1) {
        u32 e;
        if (fat_get(v, c, &e) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        if (e)
            continue;
        fsinfo_invalidate(v);
        EFI_STATUS s = write_clus(v, c, zero);
        if (s == EFI_SUCCESS)
            s = fat_set(v, c, EOC);
        if (s == EFI_SUCCESS && prev)
            s = fat_set(v, prev, c);
        if (s != EFI_SUCCESS)
            return s;
        v->next_free = c + 1;
        *out = c;
        return EFI_SUCCESS;
    }
    return EFI_VOLUME_FULL;
}

/* Free clus and the rest of its chain */
static EFI_STATUS chain_free(struct fat_vol *v, u32 clus)
{
    fsinfo_invalidate(v);
    for (u32 guard = 0; clus >= 2 && clus <= v->max_clus && guard <= v->max_clus; guard++) {
        u32 next;
        if (fat_get(v, clus, &next) != EFI_SUCCESS || fat_set(v, clus, 0) != EFI_SUCCESS)
            return EFI_DEVICE_ERROR;
        if (clus < v->next_free)
            v->next_free = clus;
        clus = next;
    }
    return EFI_SUCCESS;
}

/* The n-th cluster of a chain (0: start), or EOC past its end */
static EFI_STATUS chain_nth(struct fat_vol *v, u32 clus, u64 n, u32 *out)
{
    while (n-- && clus != EOC) {
        if (clus < 2 || clus > v->max_clus || fat_get(v, clus, &clus) != EFI_SUCCESS)
            return EFI_VOLUME_CORRUPTED;
    }
    *out = clus;
    return EFI_SUCCESS;
}

/* --- directories ----------------------------------------------------- */

/* A directory is its first cluster, or the FAT12/16 root's fixed area */
struct dir {
    u32 clus;
    int fixed_root;
};

static struct dir dir_of(struct fat_vol *v, u32 clus, int is_root)
{
    struct dir d;
    d.fixed_root = is_root && v->kind != FAT32;
    d.clus = is_root && v->kind == FAT32 ? v->root_clus : clus;
    return d;
}

/* Sector and offset of slot idx (32 bytes each); EFI_NOT_FOUND past the
 * directory's end */
static EFI_STATUS slot_where(struct fat_vol *v, struct dir d, u32 idx, u64 *lba, u32 *off)
{
    u64 byte = (u64)idx * 32;
    if (d.fixed_root) {
        if (idx >= v->root_entries)
            return EFI_NOT_FOUND;
        *lba = v->reserved + (u64)v->fats * v->fat_secs + byte / v->bps;
    } else {
        u32 c;
        if (chain_nth(v, d.clus, byte / v->clus_bytes, &c) != EFI_SUCCESS)
            return EFI_VOLUME_CORRUPTED;
        if (c == EOC || c < 2)
            return EFI_NOT_FOUND;
        *lba = clus_lba(v, c) + (byte % v->clus_bytes) / v->bps;
    }
    *off = (u32)(byte % v->bps);
    return EFI_SUCCESS;
}

static EFI_STATUS slot_read(struct fat_vol *v, struct dir d, u32 idx, u8 *e)
{
    u64 lba;
    u32 off;
    EFI_STATUS s = slot_where(v, d, idx, &lba, &off);
    if (s != EFI_SUCCESS)
        return s;
    if (v->dir_lba != lba) {
        if (bio_read(v, lba, v->dirbuf, v->bps) != EFI_SUCCESS) {
            v->dir_lba = ~0ULL;
            return EFI_DEVICE_ERROR;
        }
        v->dir_lba = lba;
    }
    memcpy(e, v->dirbuf + off, 32);
    return EFI_SUCCESS;
}

static EFI_STATUS slot_write(struct fat_vol *v, struct dir d, u32 idx, const u8 *e)
{
    u64 lba;
    u32 off;
    EFI_STATUS s = slot_where(v, d, idx, &lba, &off);
    if (s != EFI_SUCCESS)
        return s;
    if (v->dir_lba != lba && bio_read(v, lba, v->dirbuf, v->bps) != EFI_SUCCESS)
        return EFI_DEVICE_ERROR;
    memcpy(v->dirbuf + off, e, 32);
    s = bio_write(v, lba, v->dirbuf, v->bps);
    v->dir_lba = s == EFI_SUCCESS ? lba : ~0ULL;
    return s;
}

/* Long-name entries come before their 8.3 entry, last part first; each
 * holds 13 UCS-2 characters and the 8.3 name's checksum. */
static const u8 lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

struct lfn {
    CHAR16 name[LFN_MAX + 1];
    u8 csum;
    u8 next;                    /* the part expected next; 0: none pending */
    u32 first;                  /* its first slot */
};

static void lfn_add(struct lfn *l, const u8 *e, u32 idx)
{
    u8 n = e[0] & 0x1F;
    if (e[0] & 0x40) {
        memset(l->name, 0, sizeof l->name);
        l->csum = e[13];
        l->next = n;
        l->first = idx;
    }
    if (!n || n != l->next || e[13] != l->csum) {
        l->next = 0;
        return;
    }
    for (int i = 0; i < 13; i++) {
        u32 k = (u32)(n - 1) * 13 + i;
        if (k < LFN_MAX)
            l->name[k] = rd16(e + lfn_at[i]);
    }
    l->next = n - 1 ? n - 1 : 0xFF;     /* 0xFF: complete */
}

static u8 short_csum(const u8 *e)
{
    u8 c = 0;
    for (int i = 0; i < 11; i++)
        c = (u8)(((c & 1) << 7) + (c >> 1) + e[i]);
    return c;
}

/* The 8.3 entry's name as "NAME.EXT" (lower case where the entry's
 * case flags say so) */
static void short_name(const u8 *e, CHAR16 *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        CHAR16 c = i == 0 && e[0] == 0x05 ? 0xE5 : e[i];
        out[n++] = (e[12] & 0x08) && c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i];
    }
    out[n] = 0;
}

/* Walking a directory: each call gives the next file or directory entry
 * with its name (long if it has a valid one) */
struct walk {
    struct dir d;
    u32 idx;                    /* the next slot to look at */
    u32 first, slot;            /* the found entry's slots */
    struct lfn lfn;
    u8 e[32];
    CHAR16 name[LFN_MAX + 1];
};

/* 1: found, 0: the end, or an error status as -1 with *err */
static int walk_next(struct fat_vol *v, struct walk *w, EFI_STATUS *err)
{
    for (;; w->idx++) {
        EFI_STATUS s = slot_read(v, w->d, w->idx, w->e);
        if (s == EFI_NOT_FOUND)
            return 0;
        if (s != EFI_SUCCESS) {
            *err = s;
            return -1;
        }
        u8 *e = w->e;
        if (e[0] == 0)
            return 0;
        if (e[0] == 0xE5) {
            w->lfn.next = 0;
            continue;
        }
        if ((e[11] & 0x3F) == A_LFN) {
            lfn_add(&w->lfn, e, w->idx);
            continue;
        }
        int long_ok = w->lfn.next == 0xFF && w->lfn.csum == short_csum(e);
        w->lfn.next = 0;
        if (e[11] & A_LABEL)
            continue;                           /* the volume label */
        w->slot = w->idx;
        w->first = long_ok ? w->lfn.first : w->idx;
        if (long_ok) {
            int i = 0;
            for (; i < LFN_MAX && w->lfn.name[i] && w->lfn.name[i] != 0xFFFF; i++)
                w->name[i] = w->lfn.name[i];
            w->name[i] = 0;
        } else {
            short_name(e, w->name);
        }
        w->idx++;
        return 1;
    }
}

static int name_eq(const CHAR16 *a, const CHAR16 *b)
{
    for (; *a && *b; a++, b++)
        if (upcase(*a) != upcase(*b))
            return 0;
    return !*a && !*b;
}

/* Find comp in a directory: by long name or 8.3 name. The walk keeps
 * the entry */
static EFI_STATUS dir_find(struct fat_vol *v, struct dir d, const CHAR16 *comp, struct walk *w)
{
    CHAR16 alias[13];
    memset(w, 0, sizeof *w);
    w->d = d;
    EFI_STATUS err = EFI_SUCCESS;
    int r;
    while ((r = walk_next(v, w, &err)) > 0) {
        short_name(w->e, alias);
        if (name_eq(w->name, comp) || name_eq(alias, comp))
            return EFI_SUCCESS;
    }
    return r < 0 ? err : EFI_NOT_FOUND;
}

static int dir_has_short(struct fat_vol *v, struct dir d, const u8 *name11)
{
    struct walk w;
    memset(&w, 0, sizeof w);
    w.d = d;
    EFI_STATUS err;
    while (walk_next(v, &w, &err) > 0)
        if (!memcmp(w.e, name11, 11))
            return 1;
    return 0;
}

static u32 entry_clus(const u8 *e)
{
    return rd16(e + 26) | (u32)rd16(e + 20) << 16;
}

/* --- names for new entries ------------------------------------------- */

static int short_char_ok(CHAR16 c)
{
    if (c >= 'A' && c <= 'Z')
        return 1;
    if (c >= '0' && c <= '9')
        return 1;
    return c > 0x7F ? 0 : (c && c > ' ' && !(c == '"' || c == '*' || c == '+' || c == ',' ||
           c == '.' || c == '/' || c == ':' || c == ';' || c == '<' || c == '=' || c == '>' ||
           c == '?' || c == '[' || c == '\\' || c == ']' || c == '|' || (c >= 'a' && c <= 'z')));
}

static int long_char_ok(CHAR16 c)
{
    return c >= ' ' && !(c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
                         c == '>' || c == '?' || c == '\\' || c == '|');
}

/* name as an exact 8.3 name (upper case, valid characters), or 0 */
static int exact_short(const CHAR16 *name, u8 out[11])
{
    memset(out, ' ', 11);
    int i = 0, n = 0;
    for (; name[i] && name[i] != '.'; i++) {
        if (n == 8 || !short_char_ok(name[i]))
            return 0;
        out[n++] = (u8)name[i];
    }
    if (!n)
        return 0;
    if (name[i] == '.') {
        i++;
        for (n = 8; name[i]; i++) {
            if (n == 11 || !short_char_ok(name[i]))
                return 0;
            out[n++] = (u8)name[i];
        }
        if (n == 8)
            return 0;                           /* "NAME." */
    }
    return 1;
}

/* An alias for a long name: up to six characters of its base, ~N, and
 * three of its last extension, unique in the directory */
static int make_alias(struct fat_vol *v, struct dir d, const CHAR16 *name, u8 out[11])
{
    int dot = -1;
    for (int i = 0; name[i]; i++)
        if (name[i] == '.')
            dot = i;
    u8 base[8], ext[3];
    int nb = 0, ne = 0;
    for (int i = 0; name[i] && (dot < 0 || i < dot) && nb < 6; i++) {
        CHAR16 c = upcase(name[i]);
        if (c == ' ' || c == '.')
            continue;
        base[nb++] = short_char_ok(c) ? (u8)c : '_';
    }
    for (int i = dot + 1; dot >= 0 && name[i] && ne < 3; i++) {
        CHAR16 c = upcase(name[i]);
        if (c == ' ')
            continue;
        ext[ne++] = short_char_ok(c) ? (u8)c : '_';
    }
    if (!nb)
        base[nb++] = '_';
    for (u32 n = 1; n < 1000000; n++) {
        char num[8];
        int k = 0;
        for (u32 t = n; t; t /= 10)
            num[k++] = (char)('0' + t % 10);
        int keep = nb + 1 + k > 8 ? 8 - 1 - k : nb;
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        out[keep] = '~';
        for (int j = 0; j < k; j++)
            out[keep + 1 + j] = (u8)num[k - 1 - j];
        memcpy(out + 8, ext, ne);
        if (!dir_has_short(v, d, out))
            return 1;
    }
    return 0;
}

/* FAT date and time from the clock (0 if it has none) */
static void fat_now(u16 *date, u16 *time)
{
    EFI_TIME t;
    *date = *time = 0;
    EFI_STATUS (EFIAPI *get_time)(EFI_TIME *, void *) =
        (EFI_STATUS (EFIAPI *)(EFI_TIME *, void *))efi_st->runtime_services->get_time;
    if (get_time(&t, NULL) != EFI_SUCCESS || t.year < 1980 || t.year > 2107)
        return;
    *date = (u16)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (u16)(t.hour << 11 | t.minute << 5 | t.second / 2);
}

/* Find count free slots in a row (growing the directory when it runs
 * out); the first in *at */
static EFI_STATUS slots_free(struct fat_vol *v, struct dir d, u32 count, u8 *zero, u32 *at)
{
    u32 run = 0;
    for (u32 idx = 0;; idx++) {
        u8 e[32];
        EFI_STATUS s = slot_read(v, d, idx, e);
        if (s == EFI_NOT_FOUND) {
            if (d.fixed_root)
                return EFI_VOLUME_FULL;
            /* Past the last cluster: add one (zeroed: all free) */
            u32 last, c;
            if (chain_nth(v, d.clus, (u64)idx * 32 / v->clus_bytes - 1, &last) != EFI_SUCCESS)
                return EFI_VOLUME_CORRUPTED;
            s = clus_alloc(v, last, zero, &c);
            if (s != EFI_SUCCESS)
                return s;
            v->dir_lba = ~0ULL;
            idx--;
            continue;
        }
        if (s != EFI_SUCCESS)
            return s;
        if (e[0] == 0 || e[0] == 0xE5) {
            if (++run == count) {
                *at = idx + 1 - count;
                return EFI_SUCCESS;
            }
        } else {
            run = 0;
        }
    }
}

/* A new entry for name in directory d: its long-name entries, then its
 * 8.3 entry with attr, start and size. The slots used in *first, *slot */
static EFI_STATUS entry_create(struct fat_vol *v, struct dir d, const CHAR16 *name, u8 attr,
                               u32 start, u32 size, u8 *zero, u32 *first, u32 *slot)
{
    UINTN len = wcslen16(name);
    if (!len || len > LFN_MAX)
        return EFI_INVALID_PARAMETER;
    for (UINTN i = 0; i < len; i++)
        if (!long_char_ok(name[i]))
            return EFI_INVALID_PARAMETER;
    if (name[len - 1] == '.' || name[len - 1] == ' ')
        return EFI_INVALID_PARAMETER;

    u8 e[32];
    memset(e, 0, 32);
    int plain = exact_short(name, e);
    if (!plain && !make_alias(v, d, name, e))
        return EFI_ACCESS_DENIED;
    u32 parts = plain ? 0 : (u32)(len + 12) / 13;
    u32 at;
    EFI_STATUS s = slots_free(v, d, parts + 1, zero, &at);
    if (s != EFI_SUCCESS)
        return s;

    u8 csum = short_csum(e);
    for (u32 p = parts; p >= 1; p--) {          /* last part first */
        u8 l[32];
        memset(l, 0, 32);
        l[0] = (u8)(p | (p == parts ? 0x40 : 0));
        l[11] = A_LFN;
        l[13] = csum;
        for (int i = 0; i < 13; i++) {
            UINTN k = (UINTN)(p - 1) * 13 + i;
            u16 c = k < len ? name[k] : k == len ? 0 : 0xFFFF;
            wr16(l + lfn_at[i], c);
        }
        s = slot_write(v, d, at + parts - p, l);
        if (s != EFI_SUCCESS)
            return s;
    }
    u16 date, time;
    fat_now(&date, &time);
    e[11] = attr;
    wr16(e + 14, time);
    wr16(e + 16, date);
    wr16(e + 18, date);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 20, (u16)(start >> 16));
    wr16(e + 26, (u16)start);
    wr32(e + 28, size);
    s = slot_write(v, d, at + parts, e);
    *first = at;
    *slot = at + parts;
    return s;
}

static EFI_STATUS slots_delete(struct fat_vol *v, struct dir d, u32 first, u32 last)
{
    for (u32 i = first; i <= last; i++) {
        u8 e[32];
        EFI_STATUS s = slot_read(v, d, i, e);
        if (s != EFI_SUCCESS)
            return s;
        e[0] = 0xE5;
        s = slot_write(v, d, i, e);
        if (s != EFI_SUCCESS)
            return s;
    }
    return EFI_SUCCESS;
}

/* --- the file protocol ----------------------------------------------- */

static struct fat_file *file_of(EFI_FILE_PROTOCOL *this)
{
    struct fat_file *f = (struct fat_file *)this;
    return f && f->magic == FILE_MAGIC ? f : NULL;
}

static void fill_file_ops(EFI_FILE_PROTOCOL *f);

static struct fat_file *file_new(struct fat_vol *v)
{
    struct fat_file *f;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *f, (void **)&f) != EFI_SUCCESS)
        return NULL;
    memset(f, 0, sizeof *f);
    f->magic = FILE_MAGIC;
    f->vol = v;
    fill_file_ops(&f->file);
    return f;
}

static struct fat_file *root_new(struct fat_vol *v)
{
    struct fat_file *f = file_new(v);
    if (!f)
        return NULL;
    f->is_root = 1;
    f->start_clus = v->kind == FAT32 ? v->root_clus : 0;
    f->attr = A_DIR;
    f->name[0] = '\\';
    return f;
}

static void *zero_clus(struct fat_vol *v)
{
    void *z;
    if (efi_allocate_pool(EfiBootServicesData, v->clus_bytes, &z) != EFI_SUCCESS)
        return NULL;
    memset(z, 0, v->clus_bytes);
    return z;
}

/* Write the file's start cluster, size and attributes back to its entry */
static EFI_STATUS entry_update(struct fat_file *f, int modified)
{
    if (f->is_root)
        return EFI_SUCCESS;
    struct fat_vol *v = f->vol;
    struct dir d = dir_of(v, f->dir_clus, f->dir_is_root);
    u8 e[32];
    EFI_STATUS s = slot_read(v, d, f->slot, e);
    if (s != EFI_SUCCESS)
        return s;
    e[11] = f->attr;
    wr16(e + 20, (u16)(f->start_clus >> 16));
    wr16(e + 26, (u16)f->start_clus);
    wr32(e + 28, (f->attr & A_DIR) ? 0 : f->size);
    if (modified) {
        u16 date, time;
        fat_now(&date, &time);
        wr16(e + 22, time);
        wr16(e + 24, date);
        wr16(e + 18, date);
    }
    return slot_write(v, d, f->slot, e);
}

static EFI_STATUS file_resize(struct fat_file *f, u64 size)
{
    struct fat_vol *v = f->vol;
    u32 cb = v->clus_bytes;
    u32 have = (f->size + cb - 1) / cb, need = (u32)((size + cb - 1) / cb);
    if (size > 0xFFFFFFFFULL)
        return EFI_VOLUME_FULL;
    u8 *zero = zero_clus(v);
    if (!zero)
        return EFI_OUT_OF_RESOURCES;
    EFI_STATUS s = EFI_SUCCESS;
    if (!f->start_clus)
        have = 0;
    if (need > have) {
        u32 last = 0;
        if (have) {
            s = chain_nth(v, f->start_clus, have - 1, &last);
            /* bytes past the old end in its last cluster are not ours to
             * keep: they read as zero now */
            if (s == EFI_SUCCESS && f->size % cb) {
                u8 *buf = zero_clus(v);
                if (!buf) {
                    s = EFI_OUT_OF_RESOURCES;
                } else {
                    s = read_clus(v, last, buf);
                    if (s == EFI_SUCCESS) {
                        memset(buf + f->size % cb, 0, cb - f->size % cb);
                        s = write_clus(v, last, buf);
                    }
                    efi_free_pool(buf);
                }
            }
        }
        for (u32 i = have; s == EFI_SUCCESS && i < need; i++) {
            u32 c;
            s = clus_alloc(v, last, zero, &c);
            if (s == EFI_SUCCESS && !last)
                f->start_clus = c;
            last = c;
        }
    } else if (need < have) {
        if (!need) {
            s = chain_free(v, f->start_clus);
            f->start_clus = 0;
        } else {
            u32 last, rest;
            s = chain_nth(v, f->start_clus, need - 1, &last);
            if (s == EFI_SUCCESS)
                s = fat_get(v, last, &rest);
            if (s == EFI_SUCCESS)
                s = fat_set(v, last, EOC);
            if (s == EFI_SUCCESS && rest != EOC)
                s = chain_free(v, rest);
        }
    }
    efi_free_pool(zero);
    if (s == EFI_SUCCESS) {
        f->size = (u32)size;
        f->attr |= A_ARCHIVE;
        s = entry_update(f, 1);
    }
    return s;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *this)
{
    struct fat_file *f = file_of(this);
    if (!f)
        return EFI_INVALID_PARAMETER;
    f->magic = 0;
    return efi_free_pool(f);
}

static EFI_STATUS EFIAPI file_delete(EFI_FILE_PROTOCOL *this)
{
    struct fat_file *f = file_of(this);
    if (!f)
        return EFI_INVALID_PARAMETER;
    struct fat_vol *v = f->vol;
    EFI_STATUS s = EFI_WARN_DELETE_FAILURE;
    if (f->is_root || !f->writable || v->read_only)
        goto out;
    if (f->attr & A_DIR) {                      /* only when empty */
        struct walk w;
        memset(&w, 0, sizeof w);
        w.d = dir_of(v, f->start_clus, 0);
        EFI_STATUS err;
        int r;
        while ((r = walk_next(v, &w, &err)) > 0)
            if (w.e[0] != '.')
                break;
        if (r != 0)
            goto out;
    }
    struct dir d = dir_of(v, f->dir_clus, f->dir_is_root);
    if (slots_delete(v, d, f->first_slot, f->slot) != EFI_SUCCESS)
        goto out;
    if (f->start_clus)
        chain_free(v, f->start_clus);
    s = EFI_SUCCESS;
out:
    file_close(this);
    return s;
}

/* Fill info (if it fits) for an entry: its size in *need */
static EFI_STATUS put_info(UINTN *size, void *buf, const CHAR16 *name, u64 fsize, u64 phys, u8 attr)
{
    UINTN len = wcslen16(name);
    UINTN need = (UINTN)__builtin_offsetof(EFI_FILE_INFO, file_name) + (len + 1) * sizeof(CHAR16);
    if (*size < need || !buf) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    EFI_FILE_INFO *info = buf;
    memset(info, 0, need);
    info->size = need;
    info->file_size = fsize;
    info->physical_size = phys;
    info->attribute = attr & (A_SETTABLE | A_DIR);
    memcpy(info->file_name, name, (len + 1) * sizeof(CHAR16));
    *size = need;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *this, UINTN *size, void *buf)
{
    struct fat_file *f = file_of(this);
    if (!f || !size || (!buf && *size))
        return EFI_INVALID_PARAMETER;
    struct fat_vol *v = f->vol;

    if (f->attr & A_DIR) {                      /* the next entry's EFI_FILE_INFO */
        struct walk *w;
        if (efi_allocate_pool(EfiBootServicesData, sizeof *w, (void **)&w) != EFI_SUCCESS)
            return EFI_OUT_OF_RESOURCES;
        memset(w, 0, sizeof *w);
        w->d = dir_of(v, f->start_clus, f->is_root);
        w->idx = (u32)f->pos;
        EFI_STATUS err = EFI_SUCCESS, s;
        int r = walk_next(v, w, &err);
        if (r < 0) {
            s = err;
        } else if (r == 0) {
            *size = 0;
            s = EFI_SUCCESS;
        } else {
            u32 fs = (w->e[11] & A_DIR) ? 0 : rd32(w->e + 28);
            u64 phys = ((u64)fs + v->clus_bytes - 1) / v->clus_bytes * v->clus_bytes;
            s = put_info(size, buf, w->name, fs, phys, w->e[11]);
            if (s == EFI_SUCCESS)
                f->pos = w->idx;
            else
                f->pos = w->first;
        }
        efi_free_pool(w);
        return s;
    }

    if (f->pos >= f->size) {
        *size = 0;
        return EFI_SUCCESS;
    }
    UINTN want = *size;
    if (f->pos + want > f->size)
        want = (UINTN)(f->size - f->pos);
    u32 cb = v->clus_bytes;
    void *cbuf;
    if (efi_allocate_pool(EfiBootServicesData, cb, &cbuf) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    u32 clus;
    EFI_STATUS s = chain_nth(v, f->start_clus, f->pos / cb, &clus);
    u64 skip = f->pos % cb;
    u8 *dst = buf;
    UINTN left = want;
    while (s == EFI_SUCCESS && left) {
        s = read_clus(v, clus, cbuf);
        if (s != EFI_SUCCESS)
            break;
        UINTN take = cb - (UINTN)skip;
        if (take > left)
            take = left;
        memcpy(dst, (u8 *)cbuf + skip, take);
        dst += take;
        left -= take;
        skip = 0;
        if (left && (fat_get(v, clus, &clus) != EFI_SUCCESS || clus == EOC))
            s = EFI_DEVICE_ERROR;
    }
    efi_free_pool(cbuf);
    if (s != EFI_SUCCESS)
        return EFI_DEVICE_ERROR;
    f->pos += want;
    *size = want;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *this, UINTN *size, void *buf)
{
    struct fat_file *f = file_of(this);
    if (!f || !size || (!buf && *size))
        return EFI_INVALID_PARAMETER;
    struct fat_vol *v = f->vol;
    if (f->attr & A_DIR)
        return EFI_UNSUPPORTED;
    if (v->read_only)
        return EFI_WRITE_PROTECTED;
    if (!f->writable)
        return EFI_ACCESS_DENIED;
    if (!*size)
        return EFI_SUCCESS;
    u64 end = f->pos + *size;
    if (end > 0xFFFFFFFFULL)
        return EFI_VOLUME_FULL;
    EFI_STATUS s = EFI_SUCCESS;
    if (end > f->size)
        s = file_resize(f, end);                /* zero-filled up to pos */
    if (s != EFI_SUCCESS) {
        *size = 0;
        return s;
    }
    u32 cb = v->clus_bytes;
    void *cbuf;
    if (efi_allocate_pool(EfiBootServicesData, cb, &cbuf) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    u32 clus;
    s = chain_nth(v, f->start_clus, f->pos / cb, &clus);
    u64 skip = f->pos % cb;
    const u8 *src = buf;
    UINTN left = *size;
    while (s == EFI_SUCCESS && left) {
        UINTN take = cb - (UINTN)skip;
        if (take > left)
            take = left;
        if (take < cb)
            s = read_clus(v, clus, cbuf);
        if (s != EFI_SUCCESS)
            break;
        memcpy((u8 *)cbuf + skip, src, take);
        s = write_clus(v, clus, cbuf);
        src += take;
        left -= take;
        skip = 0;
        if (s == EFI_SUCCESS && left && (fat_get(v, clus, &clus) != EFI_SUCCESS || clus == EOC))
            s = EFI_DEVICE_ERROR;
    }
    efi_free_pool(cbuf);
    if (s != EFI_SUCCESS) {
        *size -= left;
        return EFI_DEVICE_ERROR;
    }
    f->pos = end;
    f->attr |= A_ARCHIVE;
    return entry_update(f, 1);
}

static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *this)
{
    return file_of(this) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;     /* written through */
}

static EFI_STATUS EFIAPI file_get_position(EFI_FILE_PROTOCOL *this, u64 *pos)
{
    struct fat_file *f = file_of(this);
    if (!f || !pos)
        return EFI_INVALID_PARAMETER;
    if (f->attr & A_DIR)
        return EFI_UNSUPPORTED;
    *pos = f->pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(EFI_FILE_PROTOCOL *this, u64 pos)
{
    struct fat_file *f = file_of(this);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->attr & A_DIR) {
        if (pos)
            return EFI_UNSUPPORTED;             /* only rewinding a directory */
        f->pos = 0;
        return EFI_SUCCESS;
    }
    f->pos = pos == ~0ULL ? f->size : pos;      /* past the end: a write fills the gap */
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(EFI_FILE_PROTOCOL *this, EFI_GUID *guid, UINTN *size, void *buf)
{
    struct fat_file *f = file_of(this);
    if (!f || !guid || !size)
        return EFI_INVALID_PARAMETER;
    if (!guid_eq(guid, &efi_file_info_guid))
        return EFI_UNSUPPORTED;
    u32 cb = f->vol->clus_bytes;
    return put_info(size, buf, f->name, f->size, ((u64)f->size + cb - 1) / cb * cb, f->attr);
}

/* SetInfo with EFI_FILE_INFO: size, attributes, a new name in the same
 * directory */
static EFI_STATUS EFIAPI file_set_info(EFI_FILE_PROTOCOL *this, EFI_GUID *guid, UINTN n, void *buf)
{
    struct fat_file *f = file_of(this);
    if (!f || !guid || !buf)
        return EFI_INVALID_PARAMETER;
    if (!guid_eq(guid, &efi_file_info_guid))
        return EFI_UNSUPPORTED;
    EFI_FILE_INFO *info = buf;
    UINTN head = (UINTN)__builtin_offsetof(EFI_FILE_INFO, file_name);
    if (n < head + sizeof(CHAR16) || info->size > n)
        return EFI_BAD_BUFFER_SIZE;
    struct fat_vol *v = f->vol;
    if (v->read_only)
        return EFI_WRITE_PROTECTED;
    if ((info->attribute & ~(u64)(A_SETTABLE | A_DIR)) ||
        !!(info->attribute & A_DIR) != !!(f->attr & A_DIR))
        return EFI_ACCESS_DENIED;
    if (f->is_root)
        return EFI_ACCESS_DENIED;

    const CHAR16 *name = info->file_name;
    EFI_STATUS s = EFI_SUCCESS;
    int same = 1;
    for (UINTN i = 0;; i++) {
        if (name[i] != f->name[i]) {
            same = 0;
            break;
        }
        if (!name[i])
            break;
    }
    if (!same) {
        for (UINTN i = 0; name[i]; i++)
            if (name[i] == '\\' || name[i] == '/')
                return EFI_UNSUPPORTED;         /* moving to another directory */
        struct dir d = dir_of(v, f->dir_clus, f->dir_is_root);
        struct walk *w;
        if (efi_allocate_pool(EfiBootServicesData, sizeof *w, (void **)&w) != EFI_SUCCESS)
            return EFI_OUT_OF_RESOURCES;
        s = dir_find(v, d, name, w);
        int clash = s == EFI_SUCCESS && w->slot != f->slot;
        efi_free_pool(w);
        if (clash)
            return EFI_ACCESS_DENIED;
        u8 *zero = zero_clus(v);
        if (!zero)
            return EFI_OUT_OF_RESOURCES;
        u32 first, slot;
        /* The new entries first, then the old ones go */
        s = entry_create(v, d, name, f->attr, f->start_clus, f->size, zero, &first, &slot);
        efi_free_pool(zero);
        if (s == EFI_SUCCESS)
            s = slots_delete(v, d, f->first_slot, f->slot);
        if (s != EFI_SUCCESS)
            return s;
        f->first_slot = first;
        f->slot = slot;
        UINTN i = 0;
        for (; name[i] && i < LFN_MAX; i++)
            f->name[i] = name[i];
        f->name[i] = 0;
    }
    if (!(f->attr & A_DIR) && info->file_size != f->size) {
        if (!f->writable)
            return EFI_ACCESS_DENIED;
        s = file_resize(f, info->file_size);
        if (s != EFI_SUCCESS)
            return s;
    }
    f->attr = (u8)((f->attr & ~A_SETTABLE) | (info->attribute & A_SETTABLE));
    return entry_update(f, 0);
}

/* A new directory's first cluster: "." and ".." */
static EFI_STATUS dir_init(struct fat_vol *v, u32 clus, u32 parent, u8 *zero)
{
    u8 *b = zero;
    u16 date, time;
    fat_now(&date, &time);
    for (int i = 0; i < 2; i++) {
        u8 *e = b + 32 * i;
        memset(e, ' ', 11);
        e[0] = '.';
        if (i)
            e[1] = '.';
        e[11] = A_DIR;
        u32 c = i ? parent : clus;
        wr16(e + 20, (u16)(c >> 16));
        wr16(e + 26, (u16)c);
        wr16(e + 22, time);
        wr16(e + 24, date);
    }
    EFI_STATUS s = write_clus(v, clus, b);
    memset(b, 0, 64);
    return s;
}

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *this, EFI_FILE_PROTOCOL **newh,
                                   CHAR16 *name, u64 mode, u64 attr)
{
    struct fat_file *parent = file_of(this);
    if (!parent || !newh || !name)
        return EFI_INVALID_PARAMETER;
    if (mode != EFI_FILE_MODE_READ && mode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) &&
        mode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
        return EFI_INVALID_PARAMETER;
    struct fat_vol *v = parent->vol;
    int write = (mode & EFI_FILE_MODE_WRITE) != 0;
    if (write && v->read_only)
        return EFI_WRITE_PROTECTED;
    if (!(parent->attr & A_DIR))
        return EFI_NOT_FOUND;

    /* Absolute from the root, or relative to this directory */
    CHAR16 *p = name;
    u32 clus = parent->start_clus;
    int is_root = parent->is_root;
    if (*p == '\\' || *p == '/') {
        clus = 0;
        is_root = 1;
    }
    while (*p == '\\' || *p == '/')
        p++;

    struct walk *w;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *w, (void **)&w) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    EFI_STATUS s = EFI_SUCCESS;
    struct fat_file *f = NULL;
    u32 start = is_root && v->kind == FAT32 ? v->root_clus : clus, size = 0;
    u8 fattr = A_DIR;
    /* An empty name opens this directory again */
    u32 dir_clus = parent->dir_clus, first = parent->first_slot, slot = parent->slot;
    int dir_root = parent->dir_is_root, found_root = is_root, created = 0;
    CHAR16 comp[LFN_MAX + 1];
    memcpy(comp, is_root ? u"\\" : parent->name, is_root ? 2 * sizeof(CHAR16) : sizeof comp);

    while (*p) {
        int i = 0;
        while (*p && *p != '\\' && *p != '/') {
            if (i == LFN_MAX) {
                s = EFI_NOT_FOUND;
                goto out;
            }
            comp[i++] = *p++;
        }
        comp[i] = 0;
        while (*p == '\\' || *p == '/')
            p++;
        if (!(fattr & A_DIR)) {
            s = EFI_NOT_FOUND;
            goto out;
        }
        if (comp[0] == '.' && !comp[1])
            continue;                           /* this directory */
        struct dir d = dir_of(v, start, found_root);
        s = dir_find(v, d, comp, w);
        if (s == EFI_NOT_FOUND && !*p && (mode & EFI_FILE_MODE_CREATE)) {
            u8 *zero = zero_clus(v);
            if (!zero) {
                s = EFI_OUT_OF_RESOURCES;
                goto out;
            }
            u32 nclus = 0;
            u8 a = (u8)((attr & (A_SETTABLE | A_DIR)) | ((attr & A_DIR) ? 0 : A_ARCHIVE));
            s = EFI_SUCCESS;
            if (a & A_DIR) {
                s = clus_alloc(v, 0, zero, &nclus);
                if (s == EFI_SUCCESS)
                    s = dir_init(v, nclus, found_root ? 0 : start, zero);
            }
            if (s == EFI_SUCCESS)
                s = entry_create(v, d, comp, a, nclus, 0, zero, &first, &slot);
            efi_free_pool(zero);
            if (s != EFI_SUCCESS)
                goto out;
            dir_clus = found_root ? 0 : start;
            dir_root = found_root;
            start = nclus;
            size = 0;
            fattr = a;
            found_root = 0;
            created = 1;
            break;
        }
        if (s != EFI_SUCCESS)
            goto out;
        dir_clus = found_root ? 0 : start;
        dir_root = found_root;
        first = w->first;
        slot = w->slot;
        start = entry_clus(w->e);
        size = rd32(w->e + 28);
        fattr = w->e[11];
        found_root = (fattr & A_DIR) && start == 0;     /* ".." back to the root */
        if (found_root)
            start = v->kind == FAT32 ? v->root_clus : 0;
        for (i = 0; w->name[i]; i++)
            comp[i] = w->name[i];
        comp[i] = 0;
    }
    if (write && !created && (fattr & A_RO)) {
        s = EFI_ACCESS_DENIED;
        goto out;
    }
    f = found_root ? root_new(v) : file_new(v);
    if (!f) {
        s = EFI_OUT_OF_RESOURCES;
        goto out;
    }
    if (!found_root) {
        f->start_clus = start;
        f->size = (fattr & A_DIR) ? 0 : size;
        f->attr = fattr;
        f->dir_clus = dir_clus;
        f->dir_is_root = dir_root;
        f->first_slot = first;
        f->slot = slot;
        memcpy(f->name, comp, sizeof f->name);
    }
    f->writable = write;
    *newh = &f->file;
    s = EFI_SUCCESS;
out:
    efi_free_pool(w);
    return s;
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
    struct fat_file *f = root_new(v);
    if (!f)
        return EFI_OUT_OF_RESOURCES;
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
    u8 *b;
    if (efi_allocate_pool(EfiBootServicesData, 3 * bsz, (void **)&b) != EFI_SUCCESS)
        return 0;
    if (bio->read_blocks(bio, bio->media->media_id, 0, bsz, b) != EFI_SUCCESS ||
        (b[0] != 0xEB && b[0] != 0xE9) || b[510] != 0x55 || b[511] != 0xAA) {
        efi_free_pool(b);
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
    u16 fsinfo = rd16(b + 48);
    if (bps != bsz || spc == 0 || (spc & (spc - 1)) || fats == 0 || reserved == 0) {
        efi_free_pool(b);
        return 0;
    }

    struct fat_vol *v;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *v, (void **)&v) != EFI_SUCCESS) {
        efi_free_pool(b);
        return 0;
    }
    memset(v, 0, sizeof *v);
    v->magic = FAT_MAGIC;
    v->handle = handle;
    v->bio = bio;
    v->media_id = bio->media->media_id;
    v->read_only = bio->media->read_only;
    v->bps = bps;
    v->spc = spc;
    v->clus_bytes = (u32)bps * spc;
    v->reserved = reserved;
    v->fats = fats;
    v->root_entries = root_ent;
    v->total_secs = tot16 ? tot16 : tot32;
    v->fatbuf = b;                              /* two sectors */
    v->dirbuf = b + 2 * bsz;
    v->fat_lba = v->dir_lba = ~0ULL;

    if (fat16 == 0) {
        v->kind = FAT32;
        v->fat_secs = fat32;
        v->root_clus = root_clus ? root_clus : 2;
        v->root_secs = 0;
        v->fsinfo = fsinfo && fsinfo != 0xFFFF && fsinfo < reserved ? fsinfo : 0;
    } else {
        v->fat_secs = fat16;
        v->root_secs = (root_ent * 32 + bps - 1) / bps;
    }
    v->data_start = reserved + fats * v->fat_secs + v->root_secs;
    u32 clusters = v->total_secs > v->data_start ? (v->total_secs - v->data_start) / spc : 0;
    if (v->kind != FAT32)
        v->kind = clusters < 4085 ? FAT12 : FAT16;
    /* Clusters the FAT has entries for, whatever the BPB claims */
    u64 entries = (u64)v->fat_secs * bps * 8 / (u32)v->kind;
    v->max_clus = clusters + 1 < entries - 1 ? clusters + 1 : (u32)(entries - 1);
    v->next_free = 2;
    v->sfs.revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    v->sfs.open_volume = open_volume;

    if (!clusters || efi_install_protocol_interface(&handle, (EFI_GUID *)&efi_simple_file_system_guid,
                                                    0, &v->sfs) != EFI_SUCCESS) {
        efi_free_pool(b);
        efi_free_pool(v);
        return 0;
    }
    printf("fs: FAT%d on %s, %u KB\n", v->kind,
           bio->media->logical_partition ? "partition" : "disk",
           (u32)((v->total_secs * (u64)bps) >> 10));
    return 1;
}
