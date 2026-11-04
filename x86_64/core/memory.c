/* SquirrelBIOS x86_64-uefi: UEFI memory services.
 * Original code.
 *
 * The memory map is a sorted array of page-aligned ranges, each with a
 * UEFI memory type. It starts from the machine's map (memmap.c) and the
 * firmware's own areas are carved out of it. AllocatePages takes pages
 * from the top of the highest free range below 4 GB, the limit of the
 * firmware's 1:1 mapping. AllocatePool takes whole pages, with a 16-byte
 * header in front of the buffer. */
#include "efi.h"

#define DESC_MAX 256
#define DESC_SIZE 48            /* bigger than the struct, as the spec allows */
#define TOP_4G 0x100000000ULL
#define PAGE 4096ULL

struct range {
    u64 start;
    u64 pages;
    u32 type;
    u64 attr;
};

static struct range map[DESC_MAX];
static int nmap;
static UINTN map_key = 1;

extern char __fw_start[], __fw_end[];

static u64 end_of(const struct range *r) { return r->start + r->pages * PAGE; }

static int free_type(u32 t) { return t == EfiConventionalMemory; }

static void merge(void)
{
    int o = 0;
    for (int i = 0; i < nmap; i++) {
        if (map[i].pages == 0)
            continue;
        if (o && map[o - 1].type == map[i].type && map[o - 1].attr == map[i].attr &&
            end_of(&map[o - 1]) == map[i].start)
            map[o - 1].pages += map[i].pages;
        else
            map[o++] = map[i];
    }
    nmap = o;
}

static int insert_at(int i, struct range r)
{
    if (nmap >= DESC_MAX)
        return 0;
    for (int j = nmap; j > i; j--)
        map[j] = map[j - 1];
    map[i] = r;
    nmap++;
    return 1;
}

/* Give [start, start + pages) the type t, wherever the map covers it.
 * Pieces of the range that no entry covered are added too. */
static int set_type(u64 start, u64 pages, u32 t, u64 attr)
{
    u64 end = start + pages * PAGE;
    /* split at start and end */
    for (int i = 0; i < nmap; i++) {
        struct range *r = &map[i];
        if (r->start < start && end_of(r) > start) {
            struct range tail = *r;
            tail.start = start;
            tail.pages = (end_of(r) - start) / PAGE;
            r->pages = (start - r->start) / PAGE;
            if (!insert_at(i + 1, tail))
                return 0;
        }
    }
    for (int i = 0; i < nmap; i++) {
        struct range *r = &map[i];
        if (r->start < end && end_of(r) > end) {
            struct range tail = *r;
            tail.start = end;
            tail.pages = (end_of(r) - end) / PAGE;
            r->pages = (end - r->start) / PAGE;
            if (!insert_at(i + 1, tail))
                return 0;
        }
    }
    /* drop what lies inside, then put the one new range in order */
    int o = 0, pos = -1;
    for (int i = 0; i < nmap; i++) {
        if (map[i].start >= start && end_of(&map[i]) <= end)
            continue;
        if (pos < 0 && map[i].start >= end)
            pos = o;
        map[o++] = map[i];
    }
    nmap = o;
    if (pos < 0)
        pos = nmap;
    struct range r = { start, pages, t, attr };
    if (!insert_at(pos, r))
        return 0;
    merge();
    map_key++;
    return 1;
}

/* All of [start, start + pages) has type t */
static int all_type(u64 start, u64 pages, int (*ok)(u32))
{
    u64 at = start, end = start + pages * PAGE;
    for (int i = 0; i < nmap && at < end; i++) {
        if (end_of(&map[i]) <= at)
            continue;
        if (map[i].start > at || !ok(map[i].type))
            return 0;
        at = end_of(&map[i]);
    }
    return at >= end;
}

static u32 from_e820(u32 type)
{
    switch (type) {
    case 1: return EfiConventionalMemory;
    case 3: return EfiACPIReclaimMemory;
    case 4: return EfiACPIMemoryNVS;
    case 5: return EfiUnusableMemory;
    default: return EfiReservedMemoryType;
    }
}

void efi_memory_init(void)
{
    nmap = 0;
    for (int i = 0; i < mem_count; i++) {
        u64 b = mem_map[i].base, e = b + mem_map[i].length;
        u32 t = from_e820(mem_map[i].type);
        if (t == EfiConventionalMemory) {       /* whole pages only */
            b = (b + PAGE - 1) & ~(PAGE - 1);
            e &= ~(PAGE - 1);
        } else {
            b &= ~(PAGE - 1);
            e = (e + PAGE - 1) & ~(PAGE - 1);
        }
        if (e > b)
            set_type(b, (e - b) / PAGE, t, t == EfiReservedMemoryType ? 0 : EFI_MEMORY_WB);
    }
    /* The firmware's own memory */
    set_type(0, 1, EfiBootServicesData, EFI_MEMORY_WB);                  /* page 0 */
    set_type(0x1000, 6, EfiBootServicesData, EFI_MEMORY_WB);             /* page tables */
    set_type(0xA0000, 0x60, EfiReservedMemoryType, 0);                   /* VGA, ROMs */
    set_type(0x100000, 0x100, EfiBootServicesData, EFI_MEMORY_WB);       /* stack */
    u64 fs = (u64)(uptr)__fw_start, fe = ((u64)(uptr)__fw_end + PAGE - 1) & ~(PAGE - 1);
    set_type(fs, (fe - fs) / PAGE, EfiBootServicesCode, EFI_MEMORY_WB);
}

static int valid_alloc_type(EFI_MEMORY_TYPE t)
{
    return (t >= EfiLoaderCode && t <= EfiRuntimeServicesData) ||
           t == EfiACPIReclaimMemory || t == EfiACPIMemoryNVS ||
           (u32)t >= 0x80000000u;
}

EFI_STATUS EFIAPI efi_allocate_pages(EFI_ALLOCATE_TYPE how, EFI_MEMORY_TYPE type,
                                     UINTN pages, EFI_PHYSICAL_ADDRESS *addr)
{
    if (!addr || !valid_alloc_type(type) || how >= MaxAllocateType)
        return EFI_INVALID_PARAMETER;
    if (pages == 0)
        return EFI_INVALID_PARAMETER;
    if (how == AllocateAddress) {
        if (*addr & (PAGE - 1))
            return EFI_INVALID_PARAMETER;
        if (*addr + pages * PAGE > TOP_4G || !all_type(*addr, pages, free_type))
            return EFI_NOT_FOUND;
        return set_type(*addr, pages, type, EFI_MEMORY_WB) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
    }
    u64 limit = TOP_4G;
    if (how == AllocateMaxAddress && *addr + 1 < limit)
        limit = (*addr + 1) & ~(PAGE - 1);
    for (int i = nmap - 1; i >= 0; i--) {
        if (!free_type(map[i].type) || map[i].start >= limit)
            continue;
        u64 top = end_of(&map[i]) < limit ? end_of(&map[i]) : limit;
        if (top - map[i].start < pages * PAGE)
            continue;
        u64 base = top - pages * PAGE;
        if (!set_type(base, pages, type, EFI_MEMORY_WB))
            return EFI_OUT_OF_RESOURCES;
        *addr = base;
        return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
}

static int freeable(u32 t)
{
    return t == EfiLoaderCode || t == EfiLoaderData || t == EfiBootServicesCode ||
           t == EfiBootServicesData || t == EfiRuntimeServicesCode ||
           t == EfiRuntimeServicesData || t == EfiACPIReclaimMemory ||
           t == EfiACPIMemoryNVS || t >= 0x80000000u;
}

EFI_STATUS EFIAPI efi_free_pages(EFI_PHYSICAL_ADDRESS addr, UINTN pages)
{
    if (addr & (PAGE - 1))
        return EFI_INVALID_PARAMETER;
    if (pages == 0 || !all_type(addr, pages, freeable))
        return EFI_NOT_FOUND;
    return set_type(addr, pages, EfiConventionalMemory, EFI_MEMORY_WB) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

EFI_STATUS EFIAPI efi_get_memory_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *buf, UINTN *key,
                                     UINTN *desc_size, u32 *desc_version)
{
    if (!size)
        return EFI_INVALID_PARAMETER;
    UINTN need = (UINTN)nmap * DESC_SIZE;
    if (desc_size)
        *desc_size = DESC_SIZE;
    if (desc_version)
        *desc_version = EFI_MEMORY_DESCRIPTOR_VERSION;
    if (*size < need) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (!buf)
        return EFI_INVALID_PARAMETER;
    for (int i = 0; i < nmap; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)buf + (usize)i * DESC_SIZE);
        memset(d, 0, DESC_SIZE);
        d->type = map[i].type;
        d->physical_start = map[i].start;
        d->virtual_start = 0;
        d->number_of_pages = map[i].pages;
        d->attribute = map[i].attr;
    }
    *size = need;
    if (key)
        *key = map_key;
    return EFI_SUCCESS;
}

#define POOL_MAGIC 0x6c6f6f5051537153ULL       /* "SqSQPool" */

EFI_STATUS EFIAPI efi_allocate_pool(EFI_MEMORY_TYPE type, UINTN size, void **out)
{
    if (!out)
        return EFI_INVALID_PARAMETER;
    UINTN pages = (size + 16 + PAGE - 1) / PAGE;
    EFI_PHYSICAL_ADDRESS a;
    EFI_STATUS s = efi_allocate_pages(AllocateAnyPages, type, pages, &a);
    if (s != EFI_SUCCESS)
        return s == EFI_NOT_FOUND ? EFI_OUT_OF_RESOURCES : s;
    u64 *h = (u64 *)(uptr)a;
    h[0] = POOL_MAGIC;
    h[1] = pages;
    *out = h + 2;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_free_pool(void *p)
{
    if (!p || ((uptr)p & (PAGE - 1)) != 16)
        return EFI_INVALID_PARAMETER;
    u64 *h = (u64 *)p - 2;
    if (h[0] != POOL_MAGIC)
        return EFI_INVALID_PARAMETER;
    h[0] = 0;
    return efi_free_pages((uptr)h, h[1]);
}

/* Exercise the services through the boot services table, the way a
 * loader calls them. Returns 1 when every check passes. */
int efi_memory_selftest(void)
{
    EFI_BOOT_SERVICES *bs = efi_st->boot_services;
    UINTN size = 0, key, dsize;
    u32 dver;
    int ok = 1;

    if (bs->get_memory_map(&size, NULL, &key, &dsize, &dver) != EFI_BUFFER_TOO_SMALL || !size)
        ok = 0, printf("efi: GetMemoryMap size query failed\n");

    EFI_PHYSICAL_ADDRESS a = 0, b = 0;
    if (bs->allocate_pages(AllocateAnyPages, EfiLoaderData, 16, &a) != EFI_SUCCESS || !a || (a & 0xFFF))
        ok = 0, printf("efi: AllocatePages failed\n");
    b = a;
    if (bs->allocate_pages(AllocateAddress, EfiLoaderData, 1, &b) != EFI_NOT_FOUND)
        ok = 0, printf("efi: AllocateAddress on used pages did not fail\n");
    if (bs->free_pages(a, 16) != EFI_SUCCESS)
        ok = 0, printf("efi: FreePages failed\n");
    if (bs->free_pages(a, 16) != EFI_NOT_FOUND)
        ok = 0, printf("efi: FreePages twice did not fail\n");
    b = 0x800000;
    if (bs->allocate_pages(AllocateAddress, EfiLoaderCode, 4, &b) != EFI_SUCCESS || b != 0x800000)
        ok = 0, printf("efi: AllocateAddress at 8 MB failed\n");
    else
        bs->free_pages(b, 4);
    b = 0xFFFFFF;
    if (bs->allocate_pages(AllocateMaxAddress, EfiBootServicesData, 2, &b) != EFI_SUCCESS || b + 0x2000 > 0x1000000)
        ok = 0, printf("efi: AllocateMaxAddress below 16 MB failed\n");
    else
        bs->free_pages(b, 2);

    void *p = NULL;
    if (bs->allocate_pool(EfiBootServicesData, 100, &p) != EFI_SUCCESS || !p || ((uptr)p & 7))
        ok = 0, printf("efi: AllocatePool failed\n");
    else {
        bs->set_mem(p, 100, 0xA5);
        if (((u8 *)p)[99] != 0xA5)
            ok = 0, printf("efi: SetMem failed\n");
        if (bs->free_pool(p) != EFI_SUCCESS)
            ok = 0, printf("efi: FreePool failed\n");
    }

    UINTN size2 = 0;
    bs->get_memory_map(&size2, NULL, &key, &dsize, &dver);
    if (size2 != size)
        ok = 0, printf("efi: memory map grew from %lu to %lu bytes\n", size, size2);
    return ok;
}
