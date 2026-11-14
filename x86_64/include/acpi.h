/* SquirrelBIOS x86_64-uefi: ACPI and SMBIOS tables. Original code.
 * Extends common/efi/include/acpi.h (the AML writer, aml.c). */
#ifndef ACPI_H
#define ACPI_H

#include "efi.h"

/* \_S5 and ResetSystem(EfiResetShutdown) agree on this: QEMU's PIIX4
 * and ICH9 power off on SLP_TYP 0. */
#define ACPI_S5_SLP_TYP 0

#define ACPI_OEM_ID     "SQRLWK"
#define ACPI_OEM_TABLE  "SQBIOS64"

/* The AML writer and the table helpers: common/efi's */
#include_next "acpi.h"
/* struct aml comes from that header; keep a forward decl for callers that
 * only see this file through another include path. */
struct aml;

/* acpi.c, smbios.c */
void acpi_init(void);
void smbios_init(void);
u8 acpi_checksum(const void *p, u32 len);
void acpi_header(u8 *t, const char *sig, u32 len, u8 rev);
void acpi_seal(u8 *t);

/* Topology-driven builders (x86_64/acpi/topo.c): firmware and the
 * host-side F1-scale check feed these the same way */
struct acpi_cpu_aff {
    u32 apic_id;
    u32 prox;                           /* proximity / NUMA node */
    int enabled;
};
struct acpi_mem_aff {
    u32 prox;
    u64 base;
    u64 length;
};
struct acpi_mcfg_entry {
    u64 base;                           /* ECAM window */
    u16 segment;
    u8 start_bus;
    u8 end_bus;
};
u32 acpi_build_madt(u8 *t, const struct acpi_cpu_aff *cpus, u32 ncpus,
                    u32 lapic_base, u8 ioapic_id, u32 ioapic_base, int all_x2);
u32 acpi_build_srat(u8 *t, const struct acpi_cpu_aff *cpus, u32 ncpus,
                    const struct acpi_mem_aff *mems, u32 nmems, int all_x2);
u32 acpi_build_mcfg(u8 *t, const struct acpi_mcfg_entry *ents, u32 n);

/* x86_64/acpi/: each adds its tables to out[] (space from take) and
 * returns how many */
typedef void *(*acpi_take_fn)(u32 len);
int acpi_numa_tables(u8 **out, acpi_take_fn take);     /* SRAT, SLIT */
int acpi_ras_tables(u8 **out, acpi_take_fn take);      /* BERT, HEST */
int acpi_cxl_tables(u8 **out, acpi_take_fn take);      /* CEDT (CHBS, CFMWS) */
int acpi_qemu_table(const char *sig, u16 *sel, u32 *off, u32 *len);  /* in etc/acpi/tables */
int acpi_qemu_bus_pxm(u8 bus);                          /* an expander's _PXM, or -1 */

/* CXL (F2): acpi_cxl_probe before the DSDT; tables after */
void acpi_cxl_probe(void);
int acpi_cxl_present(void);
int acpi_cxl_is_hb(u8 bus);                             /* a CXL root bus (_UID) */
int acpi_cxl_chbs(u8 bus, u64 *base, u64 *length);      /* CHBCR MMIO */
void acpi_cxl_dsdt(struct aml *a);                      /* ACPI0017 in \_SB */

/* mce.c: machine-check banks, read once at boot on every CPU */
#define MCE_MAX_RECORDS 32
struct mce_record {
    u32 apic_id, bank, cpuid_eax;
    u64 ctl, status, addr, misc;
};
void mce_scan(void);
u32 mce_banks(void);
u64 mce_cap(void);
int mce_records(const struct mce_record **out);
u32 mce_severity(const struct mce_record *r);   /* CPER: 0 recoverable, 1 fatal, 2 corrected */

#endif
