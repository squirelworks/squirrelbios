/* SquirrelBIOS UEFI core (common/efi): runtime services and
 * ExitBootServices. Original code. GetTime from the architecture's
 * real-time clock (seconds since 1970), ResetSystem through the
 * architecture (PSCI, SBI SRST), a small in-RAM variable store, and
 * ExitBootServices which drops boot services. */
#include "efi.h"

static int exited;

/* Everything the services call after ExitBootServices is in this file,
 * lib.c or the architecture's runtime code: on x86-64 only those stay
 * (Makefile, x86_64/link.ld) */
static int rt_guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return !memcmp(a, b, sizeof *a);
}
static u64 mono_high;
static u64 map_key_shadow;

#define VAR_MAX 32
#define VAR_NAME 32
#define VAR_DATA 256

struct efi_var {
    int used;
    CHAR16 name[VAR_NAME];
    EFI_GUID vendor;
    u32 attr;
    UINTN size;
    u8 data[VAR_DATA];
};

static struct efi_var vars[VAR_MAX];

int efi_boot_services_exited(void)
{
    return exited;
}

/* Days since 1970-01-01 to a civil date (proleptic Gregorian) */
static void civil(u64 days, u16 *y, u8 *m, u8 *d)
{
    u64 z = days + 719468;
    u64 era = z / 146097;
    u64 doe = z - era * 146097;
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u64 mp = (5 * doy + 2) / 153;
    *d = (u8)(doy - (153 * mp + 2) / 5 + 1);
    *m = (u8)(mp < 10 ? mp + 3 : mp - 9);
    *y = (u16)(yoe + era * 400 + (*m <= 2));
}

static EFI_STATUS EFIAPI get_time(EFI_TIME *time, EFI_TIME_CAPABILITIES *caps)
{
    if (!time)
        return EFI_INVALID_PARAMETER;
    u64 secs;
    if (!arch_rtc_seconds(&secs))
        return EFI_DEVICE_ERROR;
    memset(time, 0, sizeof *time);
    civil(secs / 86400, &time->year, &time->month, &time->day);
    secs %= 86400;
    time->hour = (u8)(secs / 3600);
    time->minute = (u8)(secs / 60 % 60);
    time->second = (u8)(secs % 60);
    time->timezone = 0x07FF;                    /* unspecified */
    if (caps) {
        caps->resolution = 1;
        caps->accuracy = 50000000;
        caps->sets_to_zero = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_time(EFI_TIME *time)
{
    (void)time;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI get_wakeup(BOOLEAN *enabled, BOOLEAN *pending, EFI_TIME *time)
{
    (void)enabled;
    (void)pending;
    (void)time;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI set_wakeup(BOOLEAN enable, EFI_TIME *time)
{
    (void)enable;
    (void)time;
    return EFI_UNSUPPORTED;
}

/* SetVirtualAddressMap: the runtime regions of the map the OS passes,
 * with the virtual address it gave each. Pointers into one of them move
 * by that region's offset. */
#define VMAP_MAX 16
static struct { u64 phys, end, virt; } vmap[VMAP_MAX];
static int nvmap, virtual_mode;

static int convert(u64 *p)
{
    for (int i = 0; i < nvmap; i++)
        if (*p >= vmap[i].phys && *p < vmap[i].end) {
            *p = *p - vmap[i].phys + vmap[i].virt;
            return 1;
        }
    return 0;
}

int efi_convert_pointer(void *pp)
{
    return convert((u64 *)pp);
}

static EFI_STATUS EFIAPI set_virtual_address_map(UINTN map_size, UINTN desc_size,
                                                u32 desc_ver, EFI_MEMORY_DESCRIPTOR *map)
{
    if (!exited || virtual_mode)
        return EFI_UNSUPPORTED;
    if (!map || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) || desc_ver != EFI_MEMORY_DESCRIPTOR_VERSION)
        return EFI_INVALID_PARAMETER;
    nvmap = 0;
    for (UINTN off = 0; off + desc_size <= map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)map + off);
        if (!(d->attribute & EFI_MEMORY_RUNTIME))
            continue;
        if (nvmap == VMAP_MAX)
            return EFI_OUT_OF_RESOURCES;
        vmap[nvmap].phys = d->physical_start;
        vmap[nvmap].end = d->physical_start + d->number_of_pages * 4096;
        vmap[nvmap].virt = d->virtual_start;
        nvmap++;
    }
    /* Everything the OS reaches through the tables, then the
     * architecture's own pointers (device registers) */
    EFI_RUNTIME_SERVICES *rt = efi_st->runtime_services;
    void **slot = (void **)&rt->get_time, **last = (void **)&rt->query_variable_info;
    for (; slot <= last; slot++)
        convert((u64 *)slot);
    efi_reseal(&rt->hdr);
    arch_virtual_map();
    convert((u64 *)&efi_st->runtime_services);
    convert((u64 *)&efi_st->firmware_vendor);
    convert((u64 *)&efi_st->configuration_table);
    efi_reseal(&efi_st->hdr);
    virtual_mode = 1;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI convert_pointer(UINTN debug_disposition, void **address)
{
    (void)debug_disposition;
    if (!address)
        return EFI_INVALID_PARAMETER;
    if (!*address)
        return (debug_disposition & 1) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
    if (!virtual_mode)
        return EFI_SUCCESS;                     /* still the 1:1 map */
    return convert((u64 *)address) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static int name_eq(const CHAR16 *a, const CHAR16 *b)
{
    while (*a || *b) {
        if (*a++ != *b++)
            return 0;
    }
    return 1;
}

static struct efi_var *find_var(CHAR16 *name, EFI_GUID *vendor)
{
    for (int i = 0; i < VAR_MAX; i++)
        if (vars[i].used && name_eq(vars[i].name, name) && rt_guid_eq(&vars[i].vendor, vendor))
            return &vars[i];
    return NULL;
}

static EFI_STATUS EFIAPI get_variable(CHAR16 *name, EFI_GUID *vendor, u32 *attr,
                                      UINTN *size, void *data)
{
    if (!name || !vendor || !size)
        return EFI_INVALID_PARAMETER;
    struct efi_var *v = find_var(name, vendor);
    if (!v)
        return EFI_NOT_FOUND;
    if (attr)
        *attr = v->attr;
    if (*size < v->size || !data) {
        *size = v->size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(data, v->data, v->size);
    *size = v->size;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_next_variable_name(UINTN *size, CHAR16 *name, EFI_GUID *vendor)
{
    if (!size || !name || !vendor)
        return EFI_INVALID_PARAMETER;
    int start = 0;
    if (name[0]) {
        struct efi_var *cur = find_var(name, vendor);
        if (!cur)
            return EFI_INVALID_PARAMETER;
        start = (int)(cur - vars) + 1;
    }
    for (int i = start; i < VAR_MAX; i++) {
        if (!vars[i].used)
            continue;
        UINTN need = 0;
        while (vars[i].name[need])
            need++;
        need = (need + 1) * sizeof(CHAR16);
        if (*size < need) {
            *size = need;
            return EFI_BUFFER_TOO_SMALL;
        }
        memcpy(name, vars[i].name, need);
        *vendor = vars[i].vendor;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI set_variable(CHAR16 *name, EFI_GUID *vendor, u32 attr,
                                      UINTN size, void *data)
{
    if (!name || !vendor || name[0] == 0)
        return EFI_INVALID_PARAMETER;
    if (size && !data)
        return EFI_INVALID_PARAMETER;
    if (size > VAR_DATA)
        return EFI_OUT_OF_RESOURCES;
    struct efi_var *v = find_var(name, vendor);
    if (!size) {
        if (!v)
            return EFI_NOT_FOUND;
        v->used = 0;
        return EFI_SUCCESS;
    }
    if (!v) {
        for (int i = 0; i < VAR_MAX; i++)
            if (!vars[i].used) {
                v = &vars[i];
                break;
            }
        if (!v)
            return EFI_OUT_OF_RESOURCES;
        memset(v, 0, sizeof *v);
        for (int i = 0; i < VAR_NAME - 1 && name[i]; i++)
            v->name[i] = name[i];
        v->vendor = *vendor;
        v->used = 1;
    }
    v->attr = attr;
    v->size = size;
    memcpy(v->data, data, size);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_next_high_mono(u32 *count)
{
    if (!count)
        return EFI_INVALID_PARAMETER;
    *count = (u32)(++mono_high);
    return EFI_SUCCESS;
}

static void EFIAPI reset_system(EFI_RESET_TYPE type, EFI_STATUS status, UINTN size, void *data)
{
    (void)size;
    (void)data;
    arch_reset(type, status);
}

static EFI_STATUS EFIAPI update_capsule(void *a, UINTN b, EFI_PHYSICAL_ADDRESS c)
{
    (void)a;
    (void)b;
    (void)c;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI query_capsule(void *a, UINTN b, u64 *c, EFI_RESET_TYPE *d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI query_variable_info(u32 attr, u64 *max, u64 *rem, u64 *maxsz)
{
    (void)attr;
    if (!max || !rem || !maxsz)
        return EFI_INVALID_PARAMETER;
    *max = VAR_MAX * VAR_DATA;
    *rem = *max;
    for (int i = 0; i < VAR_MAX; i++)
        if (vars[i].used)
            *rem -= vars[i].size;
    *maxsz = VAR_DATA;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_exit_boot_services(EFI_HANDLE image, UINTN map_key)
{
    (void)image;
    if (exited)
        return EFI_SUCCESS;
    /* A fresh GetMemoryMap bumps the key; the caller's key must match
     * the last one we handed out. memory.c keeps it in the descriptor
     * walk — we accept any non-zero key that equals the shadow set by
     * efi_note_map_key from GetMemoryMap. */
    if (!map_key || (map_key_shadow && map_key != map_key_shadow))
        return EFI_INVALID_PARAMETER;
    efi_signal_exit_boot_services();
    exited = 1;
    efi_st->boot_services = NULL;
    efi_reseal(&efi_st->hdr);
    printf("efi: ExitBootServices\n");
    return EFI_SUCCESS;
}

void efi_note_map_key(UINTN key)
{
    map_key_shadow = key;
}

void efi_runtime_init(EFI_RUNTIME_SERVICES *rt)
{
    rt->get_time = (void *)get_time;
    rt->set_time = (void *)set_time;
    rt->get_wakeup_time = (void *)get_wakeup;
    rt->set_wakeup_time = (void *)set_wakeup;
    rt->set_virtual_address_map = (void *)set_virtual_address_map;
    rt->convert_pointer = (void *)convert_pointer;
    rt->get_variable = (void *)get_variable;
    rt->get_next_variable_name = (void *)get_next_variable_name;
    rt->set_variable = (void *)set_variable;
    rt->get_next_high_monotonic_count = (void *)get_next_high_mono;
    rt->reset_system = (void *)reset_system;
    rt->update_capsule = (void *)update_capsule;
    rt->query_capsule_capabilities = (void *)query_capsule;
    rt->query_variable_info = (void *)query_variable_info;
}
