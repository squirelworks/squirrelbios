/* SquirrelBIOS x86_64-uefi: ACPI and SMBIOS tables. Original code. */
#ifndef ACPI_H
#define ACPI_H

#include "efi.h"

/* \_S5 and ResetSystem(EfiResetShutdown) agree on this: QEMU's PIIX4
 * and ICH9 power off on SLP_TYP 0. */
#define ACPI_S5_SLP_TYP 0

#define ACPI_OEM_ID     "SQRLWK"
#define ACPI_OEM_TABLE  "SQBIOS64"

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

/* acpi.c, smbios.c */
void acpi_init(void);
void smbios_init(void);
u8 acpi_checksum(const void *p, u32 len);

#endif
