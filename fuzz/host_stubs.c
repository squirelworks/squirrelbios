/* Host stubs for fuzzing common/efi parsers outside the firmware.
 * Original code. Provides pool and page allocation, a small protocol
 * database, quiet printf, guid_eq, arch_sync_icache, image_call/image_exit
 * (never invoke a payload entry), and a minimal EFI_SYSTEM_TABLE with
 * get_time so FAT can stamp dates without QEMU. */
#include "efi.h"
#include "host_stubs.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* Compare without linking fat.c's efi_simple_file_system_guid. */
static const EFI_GUID host_sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

#define MAX_POOLS 512
#define MAX_HANDLES 64
#define HANDLE_PROTOCOLS 8
#define HOST_RAM_PAGES 512          /* 2 MiB fake physical RAM */
#define PAGE 4096ull

const EFI_GUID efi_loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

/* Firmware image bounds for efi_image_init (image.c). */
asm(
    ".globl __fw_start\n"
    ".globl __fw_end\n"
    "__fw_start:\n"
    "  .space 64\n"
    "__fw_end:\n"
);

struct host_prot {
    EFI_GUID guid;
    void *iface;
};

struct host_handle {
    int used;
    int count;
    struct host_prot p[HANDLE_PROTOCOLS];
};

static void *pools[MAX_POOLS];
static int npools;
static struct host_handle handles[MAX_HANDLES];
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *last_sfs;

static u8 *ram;
static u8 ram_used[HOST_RAM_PAGES];

static EFI_STATUS EFIAPI stub_get_time(EFI_TIME *t, void *caps)
{
    (void)caps;
    if (!t)
        return EFI_INVALID_PARAMETER;
    t->year = 2026;
    t->month = 11;
    t->day = 15;
    t->hour = 12;
    t->minute = 0;
    t->second = 0;
    t->pad1 = 0;
    t->nanosecond = 0;
    t->timezone = 0;
    t->daylight = 0;
    t->pad2 = 0;
    return EFI_SUCCESS;
}

static EFI_RUNTIME_SERVICES stub_rt;
static EFI_SYSTEM_TABLE stub_st;
EFI_SYSTEM_TABLE *efi_st = &stub_st;

static void stubs_init(void)
{
    static int ready;
    if (ready)
        return;
    stub_rt.get_time = (void *)stub_get_time;
    stub_st.runtime_services = &stub_rt;
    if (!ram) {
        /* Page-aligned fake RAM (over-allocate, then align). */
        static u8 *ram_raw;
        ram_raw = (u8 *)malloc((size_t)(HOST_RAM_PAGES * PAGE + PAGE));
        if (ram_raw) {
            uptr a = ((uptr)ram_raw + (uptr)PAGE - 1) & ~((uptr)PAGE - 1);
            ram = (u8 *)a;
            memset(ram, 0, (size_t)(HOST_RAM_PAGES * PAGE));
        }
    }
    ready = 1;
}

static int ram_ready(void)
{
    stubs_init();
    return ram != NULL;
}

static u64 page_index(EFI_PHYSICAL_ADDRESS addr)
{
    return (addr - (EFI_PHYSICAL_ADDRESS)(uptr)ram) / PAGE;
}

static int in_ram(EFI_PHYSICAL_ADDRESS addr, UINTN pages)
{
    u64 idx;
    if (!ram || (addr & (PAGE - 1)) || pages == 0)
        return 0;
    idx = page_index(addr);
    return idx < HOST_RAM_PAGES && idx + pages <= HOST_RAM_PAGES;
}

static int all_free(EFI_PHYSICAL_ADDRESS addr, UINTN pages)
{
    u64 i, idx;
    if (!in_ram(addr, pages))
        return 0;
    idx = page_index(addr);
    for (i = 0; i < pages; i++)
        if (ram_used[idx + i])
            return 0;
    return 1;
}

static void mark(EFI_PHYSICAL_ADDRESS addr, UINTN pages, int used)
{
    u64 i, idx = page_index(addr);
    for (i = 0; i < pages; i++)
        ram_used[idx + i] = (u8)used;
}

EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *host_last_sfs(void)
{
    return last_sfs;
}

void host_reset(void)
{
    int i;
    for (i = 0; i < npools; i++)
        free(pools[i]);
    npools = 0;
    last_sfs = NULL;
    memset(handles, 0, sizeof handles);
    memset(ram_used, 0, sizeof ram_used);
}

EFI_STATUS EFIAPI efi_allocate_pool(EFI_MEMORY_TYPE type, UINTN size, void **buf)
{
    void *p;
    (void)type;
    stubs_init();
    if (!buf || size == 0)
        return EFI_INVALID_PARAMETER;
    if (npools >= MAX_POOLS)
        return EFI_OUT_OF_RESOURCES;
    p = malloc((size_t)size);
    if (!p)
        return EFI_OUT_OF_RESOURCES;
    pools[npools++] = p;
    *buf = p;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_free_pool(void *buf)
{
    int i;
    if (!buf)
        return EFI_INVALID_PARAMETER;
    for (i = 0; i < npools; i++) {
        if (pools[i] == buf) {
            pools[i] = pools[--npools];
            free(buf);
            return EFI_SUCCESS;
        }
    }
    free(buf);
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_allocate_pages(EFI_ALLOCATE_TYPE how, EFI_MEMORY_TYPE type,
                                     UINTN pages, EFI_PHYSICAL_ADDRESS *addr)
{
    UINTN i, run;
    (void)type;
    if (!addr || pages == 0 || how >= MaxAllocateType)
        return EFI_INVALID_PARAMETER;
    if (!ram_ready())
        return EFI_OUT_OF_RESOURCES;
    /* Cap so a crafted SizeOfImage cannot exhaust the host. */
    if (pages > HOST_RAM_PAGES)
        return EFI_OUT_OF_RESOURCES;

    if (how == AllocateAddress) {
        if (!all_free(*addr, pages))
            return EFI_NOT_FOUND;
        mark(*addr, pages, 1);
        return EFI_SUCCESS;
    }

    /* AllocateAnyPages / AllocateMaxAddress: first-fit from the top so
     * preferred ImageBase addresses outside RAM still fall through. */
    run = 0;
    for (i = HOST_RAM_PAGES; i > 0; i--) {
        if (ram_used[i - 1]) {
            run = 0;
            continue;
        }
        run++;
        if (run >= pages) {
            EFI_PHYSICAL_ADDRESS base =
                (EFI_PHYSICAL_ADDRESS)(uptr)(ram + (i - 1) * PAGE);
            if (how == AllocateMaxAddress && base + pages * PAGE - 1 > *addr) {
                run = 0;
                continue;
            }
            mark(base, pages, 1);
            *addr = base;
            return EFI_SUCCESS;
        }
    }
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS EFIAPI efi_free_pages(EFI_PHYSICAL_ADDRESS addr, UINTN pages)
{
    UINTN i;
    u64 idx;
    if (pages == 0 || !in_ram(addr, pages))
        return EFI_NOT_FOUND;
    idx = page_index(addr);
    for (i = 0; i < pages; i++)
        if (!ram_used[idx + i])
            return EFI_NOT_FOUND;
    mark(addr, pages, 0);
    return EFI_SUCCESS;
}

static struct host_handle *valid_handle(EFI_HANDLE h)
{
    struct host_handle *hh = (struct host_handle *)h;
    if (!hh)
        return NULL;
    if (hh < handles || hh >= handles + MAX_HANDLES)
        return NULL;
    if (((uptr)hh - (uptr)handles) % sizeof *hh)
        return NULL;
    return hh->used ? hh : NULL;
}

static struct host_prot *find_prot(struct host_handle *h, EFI_GUID *guid)
{
    int i;
    for (i = 0; i < h->count; i++)
        if (guid_eq(&h->p[i].guid, guid))
            return &h->p[i];
    return NULL;
}

EFI_STATUS EFIAPI efi_install_protocol_interface(EFI_HANDLE *handle, EFI_GUID *guid,
                                                 u32 type, void *iface)
{
    struct host_handle *h;
    int i;
    (void)type;
    stubs_init();
    if (!handle || !guid)
        return EFI_INVALID_PARAMETER;
    if (*handle) {
        h = valid_handle(*handle);
        if (!h)
            return EFI_INVALID_PARAMETER;
        if (find_prot(h, guid))
            return EFI_INVALID_PARAMETER;
        if (h->count == HANDLE_PROTOCOLS)
            return EFI_OUT_OF_RESOURCES;
    } else {
        h = NULL;
        for (i = 0; i < MAX_HANDLES; i++) {
            if (!handles[i].used) {
                h = &handles[i];
                break;
            }
        }
        if (!h)
            return EFI_OUT_OF_RESOURCES;
        memset(h, 0, sizeof *h);
        h->used = 1;
        *handle = h;
    }
    h->p[h->count].guid = *guid;
    h->p[h->count].iface = iface;
    h->count++;
    if (guid_eq(guid, &host_sfs_guid))
        last_sfs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)iface;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_uninstall_protocol_interface(EFI_HANDLE handle, EFI_GUID *guid,
                                                   void *iface)
{
    struct host_handle *h = valid_handle(handle);
    struct host_prot *p;
    if (!h || !guid)
        return EFI_INVALID_PARAMETER;
    p = find_prot(h, guid);
    if (!p || p->iface != iface)
        return EFI_NOT_FOUND;
    *p = h->p[--h->count];
    if (h->count == 0) {
        h->used = 0;
        memset(h, 0, sizeof *h);
    }
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, void **iface)
{
    struct host_handle *h = valid_handle(handle);
    struct host_prot *p;
    if (!h || !guid || !iface)
        return EFI_INVALID_PARAMETER;
    p = find_prot(h, guid);
    if (!p) {
        *iface = NULL;
        return EFI_UNSUPPORTED;
    }
    *iface = p->iface;
    return EFI_SUCCESS;
}

int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const u8 *p, *q;
    int i;
    if (!a || !b)
        return 0;
    p = (const u8 *)a;
    q = (const u8 *)b;
    for (i = 0; i < (int)sizeof(EFI_GUID); i++)
        if (p[i] != q[i])
            return 0;
    return 1;
}

void arch_sync_icache(void *p, usize n)
{
    (void)p;
    (void)n;
}

/* Never call into a fuzzed entry point. */
u64 image_call(u64 entry, void *handle, void *st, u64 *saved_sp)
{
    (void)entry;
    (void)handle;
    (void)st;
    if (saved_sp)
        *saved_sp = 0;
    return EFI_UNSUPPORTED;
}

void image_exit(u64 saved_sp, u64 status)
{
    (void)saved_sp;
    (void)status;
    abort();
}

/* Match the firmware's void printf; stay quiet under the fuzzer. */
void printf(const char *fmt, ...)
{
    va_list ap;
    (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}
