/* SquirrelBIOS UEFI core (common/efi): ACPI table store and publication.
 * Original code, written from the ACPI 6.5 specification, section 5.2
 * (RSDP, XSDT, the common table header).
 *
 * acpi_begin takes a block of ACPI reclaim memory; the architecture then
 * asks for tables (acpi_table fills the header), adds the ones the XSDT
 * lists, and acpi_publish writes the XSDT and the RSDP, checksums
 * everything and installs the RSDP as the ACPI 2.0 configuration table. */
#include "acpi.h"

static const EFI_GUID acpi20_guid =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

#define MAX_TABLES 32

static u8 *area, *area_end, *next;
static struct acpi_header *tables[MAX_TABLES];
static int ntables;

u8 acpi_checksum(const void *p, u32 len)
{
    const u8 *b = p;
    u8 sum = 0;
    while (len--)
        sum = (u8)(sum + *b++);
    return (u8)-sum;
}

int acpi_begin(u32 pages)
{
    EFI_PHYSICAL_ADDRESS a = 0;
    if (efi_allocate_pages(AllocateAnyPages, EfiACPIReclaimMemory, pages, &a) != EFI_SUCCESS)
        return 0;
    area = next = (u8 *)(uptr)a;
    area_end = area + pages * 4096;
    memset(area, 0, pages * 4096);
    ntables = 0;
    return 1;
}

/* Zeroed, 16-byte aligned, from the reclaim block */
void *acpi_alloc(u32 len)
{
    u32 step = (len + 15) & ~15u;
    if (!area || (usize)(area_end - next) < step)
        return NULL;
    u8 *p = next;
    next += step;
    return p;
}

void *acpi_table(const char *sig, u32 len, u8 rev)
{
    struct acpi_header *h = acpi_alloc(len);
    if (!h)
        return NULL;
    memcpy(h->signature, sig, 4);
    h->length = len;
    h->revision = rev;
    memcpy(h->oem_id, ACPI_OEM_ID, 6);
    memcpy(h->oem_table_id, ACPI_OEM_TABLE, 8);
    h->oem_revision = 1;
    memcpy(h->creator_id, "SQRL", 4);
    h->creator_revision = 1;
    return h;
}

void acpi_checksum_table(void *table)
{
    struct acpi_header *h = table;
    h->checksum = 0;
    h->checksum = acpi_checksum(h, h->length);
}

void acpi_add(void *table)
{
    if (table && ntables < MAX_TABLES)
        tables[ntables++] = table;
}

/* RSDP 2.0 (5.2.5.3) */
struct rsdp {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt;
    u32 length;
    u64 xsdt;
    u8 ext_checksum;
    u8 reserved[3];
} __attribute__((packed));

void *acpi_publish(void)
{
    struct acpi_header *x = acpi_table("XSDT", sizeof(struct acpi_header) + 8 * ntables, 1);
    struct rsdp *r = acpi_alloc(sizeof *r);
    if (!x || !r)
        return NULL;
    u8 *e = (u8 *)(x + 1);
    for (int i = 0; i < ntables; i++) {
        u64 a = (uptr)tables[i];
        memcpy(e + 8 * i, &a, 8);
        acpi_checksum_table(tables[i]);
    }
    acpi_checksum_table(x);

    memcpy(r->signature, "RSD PTR ", 8);
    memcpy(r->oem_id, ACPI_OEM_ID, 6);
    r->revision = 2;
    r->length = sizeof *r;
    r->xsdt = (uptr)x;
    r->checksum = acpi_checksum(r, 20);
    r->ext_checksum = acpi_checksum(r, sizeof *r);
    efi_install_configuration_table((EFI_GUID *)&acpi20_guid, r);
    return r;
}
