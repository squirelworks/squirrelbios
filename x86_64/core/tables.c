/* SquirrelBIOS x86_64-uefi: the EFI system table and the boot and runtime
 * services tables. Original code.
 *
 * Services that are not written yet answer EFI_UNSUPPORTED; their slots
 * are filled in as later steps of roadmap C1 add them. */
#include "efi.h"

static EFI_SYSTEM_TABLE st;
static EFI_BOOT_SERVICES bs;
static EFI_RUNTIME_SERVICES rt;
static CHAR16 vendor[] = u"SquirrelBIOS";
static EFI_TPL current_tpl = 4;         /* TPL_APPLICATION */

EFI_SYSTEM_TABLE *efi_st;

u32 crc32(const void *buf, usize len)
{
    const u8 *p = buf;
    u32 c = 0xFFFFFFFF;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

static void seal(EFI_TABLE_HEADER *h, u64 sig, u32 size)
{
    h->signature = sig;
    h->revision = EFI_SPEC_REVISION;
    h->header_size = size;
    h->reserved = 0;
    h->crc32 = 0;
    h->crc32 = crc32(h, size);
}

static EFI_STATUS EFIAPI unsupported(void)
{
    return EFI_UNSUPPORTED;
}

static EFI_TPL EFIAPI raise_tpl(EFI_TPL tpl)
{
    EFI_TPL old = current_tpl;
    current_tpl = tpl;
    return old;
}

static void EFIAPI restore_tpl(EFI_TPL tpl)
{
    current_tpl = tpl;
}

static EFI_STATUS EFIAPI stall(UINTN us)
{
    while (us--)
        inb(0x80);              /* about 1 us each on PC hardware */
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI calculate_crc32(void *data, UINTN size, u32 *out)
{
    if (!data || !size || !out)
        return EFI_INVALID_PARAMETER;
    *out = crc32(data, size);
    return EFI_SUCCESS;
}

static void EFIAPI copy_mem(void *d, void *s, UINTN n)
{
    u8 *dp = d;
    const u8 *sp = s;
    if (dp < sp || dp >= sp + n)
        while (n--)
            *dp++ = *sp++;
    else
        while (n--)
            dp[n] = sp[n];
}

static void EFIAPI set_mem(void *d, UINTN n, u8 v)
{
    memset(d, v, n);
}

void efi_tables_init(void)
{
    void **slot = (void **)&bs.create_event;
    void **last = (void **)&bs.create_event_ex;
    for (; slot <= last; slot++)
        *slot = (void *)unsupported;
    bs.raise_tpl = raise_tpl;
    bs.restore_tpl = restore_tpl;
    bs.allocate_pages = efi_allocate_pages;
    bs.free_pages = efi_free_pages;
    bs.get_memory_map = efi_get_memory_map;
    bs.allocate_pool = efi_allocate_pool;
    bs.free_pool = efi_free_pool;
    bs.stall = stall;
    bs.calculate_crc32 = calculate_crc32;
    bs.copy_mem = copy_mem;
    bs.set_mem = set_mem;
    bs.reserved = NULL;
    seal(&bs.hdr, EFI_BOOT_SERVICES_SIGNATURE, sizeof bs);

    slot = (void **)&rt.get_time;
    last = (void **)&rt.query_variable_info;
    for (; slot <= last; slot++)
        *slot = (void *)unsupported;
    seal(&rt.hdr, EFI_RUNTIME_SERVICES_SIGNATURE, sizeof rt);

    st.firmware_vendor = vendor;
    st.firmware_revision = 0x00020000;          /* 2.0 */
    st.boot_services = &bs;
    st.runtime_services = &rt;
    st.number_of_table_entries = 0;
    st.configuration_table = NULL;
    seal(&st.hdr, EFI_SYSTEM_TABLE_SIGNATURE, sizeof st);
    efi_st = &st;
}
