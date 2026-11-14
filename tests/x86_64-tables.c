/* Host-side ACPI table builders for F1-scale (roadmap f1-scale).
 * Original code. Feeds a synthetic CPU/NUMA/PCI topology into the same
 * builders the firmware uses (x86_64/acpi/topo_impl.inc) and writes the
 * tables as APIC.bin / SRAT.bin / MCFG.bin for tests/acpicheck.py.
 *
 *   x86_64-tables 1024cpu OUTDIR
 *   x86_64-tables 2seg    OUTDIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ACPI_OEM_ID     "SQRLWK"
#define ACPI_OEM_TABLE  "SQBIOS64"

struct acpi_cpu_aff {
    u32 apic_id;
    u32 prox;
    int enabled;
};
struct acpi_mem_aff {
    u32 prox;
    u64 base;
    u64 length;
};
struct acpi_mcfg_entry {
    u64 base;
    u16 segment;
    u8 start_bus;
    u8 end_bus;
};

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

u8 acpi_checksum(const void *p, u32 len)
{
    const u8 *b = p;
    u8 sum = 0;
    while (len--)
        sum = (u8)(sum + *b++);
    return (u8)-sum;
}

void acpi_header(u8 *t, const char *sig, u32 len, u8 rev)
{
    memcpy(t, sig, 4);
    put32(t + 4, len);
    t[8] = rev;
    t[9] = 0;
    memcpy(t + 10, ACPI_OEM_ID, 6);
    memcpy(t + 16, ACPI_OEM_TABLE, 8);
    put32(t + 24, 1);
    memcpy(t + 28, "SQRL", 4);
    put32(t + 32, 1);
}

void acpi_seal(u8 *t)
{
    u32 len = t[4] | (u32)t[5] << 8 | (u32)t[6] << 16 | (u32)t[7] << 24;
    t[9] = 0;
    t[9] = acpi_checksum(t, len);
}

#include "../x86_64/acpi/topo_impl.inc"

#define LAPIC_BASE  0xFEE00000u
#define IOAPIC_BASE 0xFEC00000u
#define NCPU_SCALE  1024
#define NNODES      4

static int write_table(const char *dir, const char *name, const u8 *t, u32 len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    if (fwrite(t, 1, len, f) != len) {
        perror(path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int mkdir_p(const char *dir)
{
    if (mkdir(dir, 0755) == 0 || errno == EEXIST)
        return 0;
    perror(dir);
    return -1;
}

static int case_1024cpu(const char *dir)
{
    static struct acpi_cpu_aff cpus[NCPU_SCALE];
    static struct acpi_mem_aff mems[NNODES];
    static u8 madt[44 + NCPU_SCALE * 16 + 12 + 5 * 10 + 6 + 12];
    static u8 srat[48 + NCPU_SCALE * 24 + NNODES * 40];
    static u8 mcfg[44 + 16];

    for (u32 i = 0; i < NCPU_SCALE; i++) {
        cpus[i].apic_id = i;
        cpus[i].prox = i / (NCPU_SCALE / NNODES);
        cpus[i].enabled = 1;
    }
    for (u32 n = 0; n < NNODES; n++) {
        mems[n].prox = n;
        mems[n].base = (u64)n * (256ull << 20);
        mems[n].length = 256ull << 20;
    }
    struct acpi_mcfg_entry e = {
        .base = 0xB0000000ull,
        .segment = 0,
        .start_bus = 0,
        .end_bus = 0xFF,
    };

    u32 mlen = acpi_build_madt(madt, cpus, NCPU_SCALE, LAPIC_BASE, 0, IOAPIC_BASE, 1);
    u32 slen = acpi_build_srat(srat, cpus, NCPU_SCALE, mems, NNODES, 1);
    u32 clen = acpi_build_mcfg(mcfg, &e, 1);
    if (write_table(dir, "APIC.bin", madt, mlen) ||
        write_table(dir, "SRAT.bin", srat, slen) ||
        write_table(dir, "MCFG.bin", mcfg, clen))
        return 1;
    printf("tables: 1024cpu APIC=%u SRAT=%u MCFG=%u -> %s\n", mlen, slen, clen, dir);
    return 0;
}

static int case_2seg(const char *dir)
{
    static struct acpi_cpu_aff cpus[2];
    static struct acpi_mem_aff mems[1];
    static u8 madt[44 + 2 * 16 + 12 + 5 * 10 + 6 + 12];
    static u8 srat[48 + 2 * 24 + 40];
    static u8 mcfg[44 + 32];

    for (u32 i = 0; i < 2; i++) {
        cpus[i].apic_id = i;
        cpus[i].prox = 0;
        cpus[i].enabled = 1;
    }
    mems[0].prox = 0;
    mems[0].base = 0;
    mems[0].length = 128ull << 20;
    struct acpi_mcfg_entry ents[2] = {
        { .base = 0xB0000000ull, .segment = 0, .start_bus = 0, .end_bus = 0x7F },
        { .base = 0xA0000000ull, .segment = 1, .start_bus = 0, .end_bus = 0x3F },
    };

    u32 mlen = acpi_build_madt(madt, cpus, 2, LAPIC_BASE, 0, IOAPIC_BASE, 0);
    u32 slen = acpi_build_srat(srat, cpus, 2, mems, 1, 0);
    u32 clen = acpi_build_mcfg(mcfg, ents, 2);
    if (write_table(dir, "APIC.bin", madt, mlen) ||
        write_table(dir, "SRAT.bin", srat, slen) ||
        write_table(dir, "MCFG.bin", mcfg, clen))
        return 1;
    printf("tables: 2seg APIC=%u SRAT=%u MCFG=%u -> %s\n", mlen, slen, clen, dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s 1024cpu|2seg OUTDIR\n", argv[0]);
        return 2;
    }
    if (mkdir_p(argv[2]))
        return 1;
    if (!strcmp(argv[1], "1024cpu"))
        return case_1024cpu(argv[2]);
    if (!strcmp(argv[1], "2seg"))
        return case_2seg(argv[2]);
    fprintf(stderr, "%s: unknown case %s\n", argv[0], argv[1]);
    return 2;
}
