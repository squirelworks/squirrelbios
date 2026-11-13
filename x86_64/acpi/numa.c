/* SquirrelBIOS x86_64-uefi: NUMA tables, SRAT and SLIT (roadmap F1/F2).
 * Original code, written from the ACPI 6.5 specification (5.2.16 SRAT,
 * 5.2.17 SLIT) and QEMU's documented fw_cfg items.
 *
 * QEMU started with -numa gives FW_CFG_NUMA (0x0D): the node count, then
 * one node number per APIC ID below FW_CFG_MAX_CPUS (0x0F, the APIC ID
 * limit), then each node's memory size, all 64-bit little-endian. Node
 * memory follows guest RAM in order: node 0 first, the RAM below the PCI
 * hole before the RAM from 4 GB. SRAT gets a memory affinity entry per
 * contiguous piece, and a local APIC (or, past ID 254, x2APIC) affinity
 * entry per processor smp.c brought up.
 *
 * QEMU does not pass -numa dist through fw_cfg, so SLIT says 10 for a
 * node to itself and 20 to others, unless a file
 * opt/org.squirrelworks/numa-distance gives the N x N bytes. */
#include "acpi.h"
#include "smp.h"

#define FW_CFG_NUMA     0x0D
#define FW_CFG_MAX_CPUS 0x0F
#define FWCFG_DIST      "opt/org.squirrelworks/numa-distance"
#define MAX_NODES       64
#define MAX_IDS         4096

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

static u32 nodes;
static u64 node_mem[MAX_NODES];
static u8 node_of_id[MAX_IDS];

static int read_numa(void)
{
    u64 n = 0;
    u16 limit = 0;
    if (!fwcfg_present())
        return 0;
    fwcfg_read(FW_CFG_NUMA, &n, 8);
    if (n == 0 || n > MAX_NODES)
        return 0;
    fwcfg_read(FW_CFG_MAX_CPUS, &limit, 2);
    /* the item again from its start: count, per-ID nodes, sizes */
    outw(0x510, FW_CFG_NUMA);
    u64 v;
    for (int i = 0; i < 8; i++)
        inb(0x511);
    for (u32 id = 0; id < limit; id++) {
        u8 *b = (u8 *)&v;
        for (int i = 0; i < 8; i++)
            b[i] = inb(0x511);
        if (id < MAX_IDS)
            node_of_id[id] = v < n ? (u8)v : 0;
    }
    for (u32 i = 0; i < n; i++) {
        u8 *b = (u8 *)&node_mem[i];
        for (int k = 0; k < 8; k++)
            b[k] = inb(0x511);
    }
    nodes = (u32)n;
    return 1;
}

static u64 low_top(void)
{
    u64 top = 0;
    for (int i = 0; i < mem_count; i++) {
        u64 end = mem_map[i].base + mem_map[i].length;
        if (mem_map[i].type == 1 && end <= 0x100000000ull && end > top)
            top = end;
    }
    return top;
}

/* Memory affinity entries for RAM offsets [start, start + len) of node */
static u8 *mem_entries(u8 *p, u32 node, u64 start, u64 len, u64 low)
{
    while (len) {
        u64 base, piece;
        if (start < low) {
            base = start;
            piece = low - start < len ? low - start : len;
        } else {
            base = 0x100000000ull + (start - low);
            piece = len;
        }
        p[0] = 1;                       /* memory affinity */
        p[1] = 40;
        put32(p + 2, node);
        put64(p + 8, base);
        put64(p + 16, piece);
        put32(p + 28, 1);               /* enabled */
        p += 40;
        start += piece;
        len -= piece;
    }
    return p;
}

int acpi_numa_tables(u8 **out, acpi_take_fn take)
{
    if (!read_numa())
        return 0;
    int cpus = smp_cpu_count();
    u8 *srat = take(48 + (u32)cpus * 24 + nodes * 2 * 40);
    if (!srat)
        return 0;
    u8 *p = srat + 48;
    put32(srat + 36, 1);                /* reserved, 1 for compatibility */
    for (int i = 0; i < cpus; i++) {
        const struct smp_cpu_info *ci = smp_cpu(i);
        u32 id = ci->apic_id;
        u32 node = id < MAX_IDS ? node_of_id[id] : 0;
        if (id < 255) {
            p[0] = 0;                   /* processor local APIC affinity */
            p[1] = 16;
            p[2] = (u8)node;
            p[3] = (u8)id;
            put32(p + 4, ci->enabled ? 1 : 0);
            p[8] = 0;
            p[9] = (u8)(node >> 8);
            p[10] = (u8)(node >> 16);
            p[11] = (u8)(node >> 24);
            put32(p + 12, 0);
            p += 16;
        } else {
            p[0] = 2;                   /* processor local x2APIC affinity */
            p[1] = 24;
            put32(p + 4, node);
            put32(p + 8, id);
            put32(p + 12, ci->enabled ? 1 : 0);
            put32(p + 16, 0);
            p += 24;
        }
    }
    u64 low = low_top(), at = 0;
    for (u32 n = 0; n < nodes; n++) {
        if (node_mem[n])
            p = mem_entries(p, n, at, node_mem[n], low);
        at += node_mem[n];
    }
    u32 len = (u32)(p - srat);
    acpi_header(srat, "SRAT", len, 3);
    acpi_seal(srat);

    u32 slen = 44 + nodes * nodes;
    u8 *slit = take(slen);
    if (!slit) {
        out[0] = srat;
        return 1;
    }
    put64(slit + 36, nodes);
    u16 sel;
    u32 fsize;
    if (fwcfg_find(FWCFG_DIST, &sel, &fsize) && fsize == nodes * nodes) {
        fwcfg_read(sel, slit + 44, fsize);
    } else {
        for (u32 i = 0; i < nodes; i++)
            for (u32 j = 0; j < nodes; j++)
                slit[44 + i * nodes + j] = i == j ? 10 : 20;
    }
    acpi_header(slit, "SLIT", slen, 1);
    acpi_seal(slit);
    out[0] = srat;
    out[1] = slit;
    printf("numa: %u nodes:", nodes);
    for (u32 n = 0; n < nodes; n++)
        printf(" %lu MB", node_mem[n] >> 20);
    printf("\n");
    return 2;
}
