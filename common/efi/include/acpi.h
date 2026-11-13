/* SquirrelBIOS UEFI core (common/efi): ACPI table building. Original code.
 * acpi.c keeps the tables in ACPI reclaim memory, fills their headers and
 * checksums, and publishes an XSDT and RSDP as the ACPI 2.0
 * configuration table; aml.c writes AML for a DSDT. An architecture
 * builds its own tables with these (arm/uefi/acpi.c). */
#ifndef ACPI_H
#define ACPI_H

#include "efi.h"

#define ACPI_OEM_ID     "SQRLWK"
#define ACPI_OEM_TABLE  "SQBIOS  "

struct acpi_header {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    char creator_id[4];
    u32 creator_revision;
} __attribute__((packed));

/* Generic Address Structure (5.2.3.2) */
struct acpi_gas {
    u8 space;                   /* 0 system memory */
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;             /* 1 byte .. 4 qword */
    u64 address;
} __attribute__((packed));

/* acpi.c */
int acpi_begin(u32 pages);
void *acpi_table(const char *sig, u32 len, u8 rev);
void *acpi_alloc(u32 len);
void acpi_add(void *table);                     /* to the XSDT */
void acpi_checksum_table(void *table);
u8 acpi_checksum(const void *p, u32 len);
void *acpi_publish(void);                       /* the RSDP */

/* aml.c */
#define AML_DEPTH 16
struct aml {
    u8 *buf;
    u32 cap, len;
    u32 stack[AML_DEPTH];
    int depth;
    int overflow;
};

struct res {
    u8 buf[256];
    u32 len;
};

void aml_init(struct aml *a, u8 *buf, u32 cap);
void aml_byte(struct aml *a, u8 b);
void aml_bytes(struct aml *a, const void *p, u32 n);
void aml_u16(struct aml *a, u16 v);
void aml_u32(struct aml *a, u32 v);
void aml_open(struct aml *a);
void aml_close(struct aml *a);
void aml_name(struct aml *a, const char *path);
void aml_int(struct aml *a, u64 v);
void aml_string(struct aml *a, const char *s);
u32 aml_eisaid(const char *id);
void aml_name_int(struct aml *a, const char *name, u64 v);
void aml_name_string(struct aml *a, const char *name, const char *s);
void aml_name_eisaid(struct aml *a, const char *name, const char *id);
void aml_scope(struct aml *a, const char *name);
void aml_device(struct aml *a, const char *name);
void aml_method(struct aml *a, const char *name, u8 args);
void aml_package(struct aml *a, u8 count);
void aml_end(struct aml *a);
void aml_name_resources(struct aml *a, const char *name, const struct res *r);

void res_io(struct res *r, u16 base, u8 len);
void res_irq(struct res *r, u8 irq);
void res_mem32_fixed(struct res *r, u32 base, u32 len, int writable);
void res_interrupt_level_high(struct res *r, u32 gsi);
void res_word(struct res *r, u8 type, u16 min, u16 max);
void res_dword_mem(struct res *r, u32 min, u32 max, int cacheable);
void res_qword_mem(struct res *r, u64 min, u64 max, int cacheable);
void res_interrupt(struct res *r, u32 gsi, int edge, int active_low);
void res_dword_io_translated(struct res *r, u32 min, u32 max, u32 cpu);

#endif
