/* SquirrelBIOS x86_64-uefi: runtime services and ExitBootServices.
 * Original code. GetTime from the CMOS RTC, ResetSystem through the
 * QEMU/Bochs ACPI port and the keyboard controller, a small in-RAM
 * variable store, and ExitBootServices which drops boot services. */
#include "acpi.h"

/* Built into the runtime pages (Makefile, link.ld): everything the
 * services below call after ExitBootServices must be in this file or
 * lib.c. Boot-time-only calls are marked. */
static int exited;
static u16 rt_pm_base;                  /* chipset()->pm_base, kept for after exit */

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

static u8 cmos(u8 idx)
{
    outb(0x70, idx);
    return inb(0x71);
}

static u8 bcd(u8 v)
{
    return (u8)((v >> 4) * 10 + (v & 0x0F));
}

static EFI_STATUS EFIAPI get_time(EFI_TIME *time, EFI_TIME_CAPABILITIES *caps)
{
    if (!time)
        return EFI_INVALID_PARAMETER;
    /* Wait for update-in-progress to clear. */
    for (int i = 0; i < 10000; i++)
        if (!(cmos(0x0A) & 0x80))
            break;
    u8 sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    u8 day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    u8 century = cmos(0x32);
    if (!(cmos(0x0B) & 0x04)) {                 /* BCD mode */
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
        century = bcd(century);
    }
    memset(time, 0, sizeof *time);
    time->second = sec;
    time->minute = min;
    time->hour = hour;
    time->day = day ? day : 1;
    time->month = (mon >= 1 && mon <= 12) ? mon : 1;
    time->year = (u16)(century * 100 + year);
    if (time->year < 1980 || time->year > 2099)
        time->year = (u16)(2000 + (year % 100));
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

static EFI_STATUS EFIAPI set_virtual_address_map(UINTN map_size, UINTN desc_size,
                                                u32 desc_ver, EFI_MEMORY_DESCRIPTOR *map)
{
    (void)map_size;
    (void)desc_size;
    (void)desc_ver;
    (void)map;
    return exited ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI convert_pointer(UINTN debug_disposition, void **address)
{
    (void)debug_disposition;
    if (!exited)
        return EFI_UNSUPPORTED;
    if (!address || !*address)
        return EFI_INVALID_PARAMETER;
    return EFI_SUCCESS;                         /* identity map still */
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
    (void)status;
    (void)size;
    (void)data;
    if (type == EfiResetShutdown) {
        /* isa-debug-exit if the smoke test attached one at 0xF4. */
        outb(0xF4, 0x00);
        /* SLP_EN with the SLP_TYP the DSDT gives \_S5 (0: QEMU's soft
         * off on both PIIX4 and ICH9), on the PM block chipset.c opened. */
        u16 pm1 = exited ? rt_pm_base : chipset()->pm_base;   /* boot data before exit */
        if (pm1)
            outw((u16)(pm1 + 4), 0x2000 | (ACPI_S5_SLP_TYP << 10));
        else
            outw(0x604, 0x2000 | (ACPI_S5_SLP_TYP << 10));  /* QEMU's default PM block */
        outw(0xB004, 0x2000);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    /* Warm/cold: pulse the keyboard-controller reset line, then 0xCF9. */
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    for (;;)
        __asm__ volatile("cli; hlt");
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
    rt_pm_base = chipset()->pm_base;
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
