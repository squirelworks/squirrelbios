/* Host stubs for fuzzing common/efi/fat.c outside the firmware.
 * Original code. Provides pool allocation, protocol install, a quiet
 * printf, guid_eq, and a minimal EFI_SYSTEM_TABLE with get_time so the
 * FAT writer can stamp dates without QEMU or a real RTC. */
#include "efi.h"
#include "host_stubs.h"

#include <stdlib.h>
#include <stdarg.h>

#define MAX_POOLS 512

static void *pools[MAX_POOLS];
static int npools;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *last_sfs;

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
    ready = 1;
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

EFI_STATUS EFIAPI efi_install_protocol_interface(EFI_HANDLE *handle, EFI_GUID *guid,
                                                 u32 type, void *iface)
{
    (void)handle;
    (void)guid;
    (void)type;
    stubs_init();
    last_sfs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)iface;
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

/* Match the firmware's void printf; stay quiet under the fuzzer. */
void printf(const char *fmt, ...)
{
    va_list ap;
    (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}
