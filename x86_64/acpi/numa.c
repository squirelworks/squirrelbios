/* SquirrelBIOS x86_64-uefi: NUMA tables, SRAT, SLIT and HMAT (roadmap F1).
 * Original code, written from the ACPI 6.5 specification (5.2.16 SRAT,
 * 5.2.17 SLIT, 5.2.28 HMAT) and QEMU's documented fw_cfg items.
 *
 * QEMU started with -numa gives FW_CFG_NUMA (0x0D): the node count, then
 * one node number per APIC ID below FW_CFG_MAX_CPUS (0x0F, the APIC ID
 * limit), then each node's memory size, all 64-bit little-endian. Node
 * memory follows guest RAM in order: node 0 first, the RAM below the PCI
 * hole before the RAM from 4 GB. SRAT gets a memory affinity entry per
 * contiguous piece, and a local APIC (or, past ID 254, x2APIC) affinity
 * entry per processor smp.c brought up.
 *
 * SLIT: the N x N bytes of a file opt/org.squirrelworks/numa-distance;
 * else the distances of QEMU's own SLIT (-numa dist), found in the
 * tables it offers in etc/acpi/tables; else 10 for a node to itself and
 * 20 to others.
 *
 * HMAT (ACPI 6.5 5.2.28, revision 2): QEMU's own structures when it has
 * an HMAT (-machine hmat=on with -numa hmat-lb and hmat-cache); else a
 * memory proximity domain attributes structure per node with memory,
 * naming its initiator. QEMU numbers proximity domains by node, as the
 * SRAT here does, so its structures fit this SRAT. */
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

/* One of QEMU's own ACPI tables, from the blob it offers to firmware
 * that loads them (etc/acpi/tables): its offset and length there */
int acpi_qemu_table(const char *sig, u16 *sel, u32 *off, u32 *len)
{
    u32 size;
    if (!fwcfg_find("etc/acpi/tables", sel, &size))
        return 0;
    for (u32 at = 0; at + 8 <= size;) {
        u8 h[8];
        fwcfg_read_at(*sel, at, h, 8);
        u32 l = h[4] | h[5] << 8 | h[6] << 16 | (u32)h[7] << 24;
        if (l < 36 || l > size - at)
            break;
        if (!memcmp(h, sig, 4)) {
            *off = at;
            *len = l;
            return 1;
        }
        at += l;
    }
    return 0;
}

/* HMAT: QEMU's structures when it has an HMAT (latency, bandwidth and
 * memory-side caches as given with -numa hmat-lb/hmat-cache); else one
 * memory proximity domain attributes structure per node with memory,
 * naming the node's own CPUs, or the nearest node with CPUs by the SLIT,
 * as its initiator */
static u8 *build_hmat(acpi_take_fn take, const u8 *dist, const int *has_cpu)
{
    u16 sel;
    u32 off, qlen;
    int qemu = acpi_qemu_table("HMAT", &sel, &off, &qlen) && qlen >= 40;
    u32 len = qemu ? qlen : 40 + nodes * 40;
    u8 *h = take(len);
    if (!h)
        return NULL;
    u8 *p = h + 40;
    if (qemu) {
        fwcfg_read_at(sel, off + 40, p, qlen - 40);
        p += qlen - 40;
    } else {
        for (u32 m = 0; m < nodes; m++) {
            if (!node_mem[m])
                continue;
            int init = -1;
            for (u32 i = 0; i < nodes; i++)
                if (has_cpu[i] && (init < 0 || dist[m * nodes + i] < dist[m * nodes + (u32)init] ||
                                   (i == m && dist[m * nodes + i] == dist[m * nodes + (u32)init])))
                    init = (int)i;
            memset(p, 0, 40);
            put16(p, 0);                /* memory proximity domain attributes */
            put32(p + 4, 40);
            put16(p + 8, init >= 0 ? 1 : 0);    /* initiator domain valid */
            put32(p + 12, init >= 0 ? (u32)init : 0);
            put32(p + 16, m);
            p += 40;
        }
    }
    put32(h + 36, 0);
    acpi_header(h, "HMAT", (u32)(p - h), 2);
    acpi_seal(h);
    printf("numa: HMAT %s\n", qemu ? "from QEMU (-numa hmat-lb)" : "with the initiator of each node");
    return h;
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
    u32 qoff, qlen;
    u64 qn = 0;
    if (fwcfg_find(FWCFG_DIST, &sel, &fsize) && fsize == nodes * nodes) {
        fwcfg_read(sel, slit + 44, fsize);
    } else if (acpi_qemu_table("SLIT", &sel, &qoff, &qlen) && qlen == slen &&
               (fwcfg_read_at(sel, qoff + 36, &qn, 8), qn == nodes)) {
        fwcfg_read_at(sel, qoff + 44, slit + 44, nodes * nodes);     /* -numa dist */
    } else {
        for (u32 i = 0; i < nodes; i++)
            for (u32 j = 0; j < nodes; j++)
                slit[44 + i * nodes + j] = i == j ? 10 : 20;
    }
    acpi_header(slit, "SLIT", slen, 1);
    acpi_seal(slit);
    out[0] = srat;
    out[1] = slit;
    int has_cpu[MAX_NODES] = { 0 };
    for (int i = 0; i < cpus; i++) {
        u32 id = smp_cpu(i)->apic_id;
        has_cpu[id < MAX_IDS ? node_of_id[id] : 0] = 1;
    }
    u8 *hmat = build_hmat(take, slit + 44, has_cpu);
    printf("numa: %u nodes:", nodes);
    for (u32 n = 0; n < nodes; n++)
        printf(" %lu MB", node_mem[n] >> 20);
    printf("\n");
    if (!hmat)
        return 2;
    out[2] = hmat;
    return 3;
}

/* The proximity domain QEMU gives an expander's root bus (-device pxb or
 * pxb-pcie with numa_node=): the _PXM in Device (PCxx) of its own DSDT,
 * or -1. Its AML is a Device op (5B 82), a PkgLength, the name PCxx,
 * then names; _PXM is a byte, word or dword integer, or Zero or One. */
int acpi_qemu_bus_pxm(u8 bus)
{
    u16 sel;
    u32 off, len;
    if (!acpi_qemu_table("DSDT", &sel, &off, &len))
        return -1;
    u8 *d;
    if (efi_allocate_pool(EfiBootServicesData, len, (void **)&d))
        return -1;
    fwcfg_read_at(sel, off, d, len);
    static const char hex[] = "0123456789ABCDEF";
    char name[4] = { 'P', 'C', hex[bus >> 4], hex[bus & 15] };
    int pxm = -1;
    for (u32 i = 36; pxm < 0 && i + 3 < len; i++) {
        if (d[i] != 0x5B || d[i + 1] != 0x82)
            continue;
        u32 lead = d[i + 2], nbytes = lead >> 6, pkg = nbytes ? lead & 15 : lead & 63;
        for (u32 k = 0; k < nbytes; k++)
            pkg |= (u32)d[i + 3 + k] << (4 + 8 * k);
        u32 at = i + 3 + nbytes, end = i + 2 + pkg;
        if (end > len || at + 4 > end || memcmp(d + at, name, 4))
            continue;
        for (u32 j = at + 4; j + 6 <= end; j++) {
            if (d[j] != 0x08 || memcmp(d + j + 1, "_PXM", 4))
                continue;
            u8 op = d[j + 5];
            if (op == 0x00 || op == 0x01)
                pxm = op;
            else if (op == 0x0A && j + 7 <= end)
                pxm = d[j + 6];
            else if (op == 0x0B && j + 8 <= end)
                pxm = d[j + 6] | d[j + 7] << 8;
            else if (op == 0x0C && j + 10 <= end)
                pxm = (int)((u32)d[j + 6] | (u32)d[j + 7] << 8 | (u32)d[j + 8] << 16 | (u32)(d[j + 9] & 0x7F) << 24);
            break;
        }
        break;
    }
    efi_free_pool(d);
    return pxm;
}

