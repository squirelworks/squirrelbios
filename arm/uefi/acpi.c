/* SquirrelBIOS aarch64-uefi: ACPI tables for an Arm server OS (roadmap
 * D1, the SBBR table set). Original code, written from the ACPI 6.5
 * specification (5.2.9 FADT, 5.2.12 MADT with the GIC structures,
 * 5.2.25 GTDT), Microsoft's SPCR description, the PCI Firmware
 * specification's MCFG, and the Arm Base Boot Requirements.
 *
 * Everything comes from the device tree QEMU built: CPUs and their
 * MPIDRs, the GIC (v2 with v2m MSI frames, or v3 with redistributors and
 * ITS), the PSCI conduit, the architected timer's interrupts, the PL011,
 * fw_cfg, the virtio-mmio transports and the generic ECAM PCIe host with
 * its windows and INTx routing. The tables:
 *   FACP  hardware-reduced, PSCI compliant (hvc or smc)
 *   APIC  GICC per CPU, GICD, GICR or GIC MSI frames, GIC ITS
 *   GTDT  the EL1/EL2 timer interrupts
 *   SPCR  the PL011 console
 *   DBG2  the PL011 as the debug port
 *   MCFG  the ECAM window
 *   IORT  PCI requester IDs to the ITS (GICv3 with an ITS)
 *   DSDT  CPUs, the PL011, fw_cfg, virtio-mmio and the PCIe host
 *         bridge (_CRS windows, _PRT)
 * Linux uses them instead of the device tree when given acpi=force. */
#include "acpi.h"

#define MAX_CPUS 512

static u32 gsi_of(const u8 *cell)       /* <type num flags> */
{
    return be32(cell) == 1 ? 16 + be32(cell + 4) : 32 + be32(cell + 4);
}

static u32 timer_flags(const u8 *cell)
{
    u32 t = be32(cell + 8) & 0xF;
    return (t & 3 ? 1 : 0) | (t & 0xA ? 2 : 0) | 4;    /* edge, active low, always-on */
}

static int node_irq(int node, u32 *gsi, int *edge, int *low)
{
    u32 len;
    const u8 *p = fdt_prop(node, "interrupts", &len);
    if (!p || len < 12)
        return 0;
    u32 t = be32(p + 8) & 0xF;
    *gsi = gsi_of(p);
    *edge = (t & 3) != 0;
    *low = (t & 0xA) != 0;
    return 1;
}

static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put16(u8 *p, u16 v) { memcpy(p, &v, 2); }

/* ------------------------------------------------------------ CPUs */

static u64 mpidr[MAX_CPUS];
static int ncpus;

static void find_cpus(void)
{
    int cpus = fdt_path("/cpus");
    if (cpus < 0)
        return;
    int depth = 0;
    for (int n = fdt_next_node(cpus, &depth); n >= 0 && depth > 0; n = fdt_next_node(n, &depth)) {
        const char *t = fdt_prop(n, "device_type", NULL);
        u64 reg;
        if (depth == 1 && t && !strcmp(t, "cpu") && fdt_reg(n, 0, &reg, NULL) && ncpus < MAX_CPUS)
            mpidr[ncpus++] = reg;
    }
}

/* ------------------------------------------------------------ MADT */

static void *madt(int gic, int v3)
{
    u64 gicd = 0, b1 = 0, s1 = 0, gich = 0, gicv = 0;
    fdt_reg(gic, 0, &gicd, NULL);
    fdt_reg(gic, 1, &b1, &s1);
    fdt_reg(gic, v3 ? 3 : 2, &gich, NULL);
    fdt_reg(gic, v3 ? 4 : 3, &gicv, NULL);
    u32 len, maint = 0;
    const u8 *ip = fdt_prop(gic, "interrupts", &len);
    if (ip && len >= 12)
        maint = gsi_of(ip);
    u32 pmu = 0;
    int pn = fdt_find_compatible("arm,armv8-pmuv3", -1);
    if (pn >= 0 && (ip = fdt_prop(pn, "interrupts", &len)) && len >= 12)
        pmu = gsi_of(ip);

    /* the frames and ITSes below the GIC node */
    int frames[16], nframes = 0;
    const char *sub = v3 ? "arm,gic-v3-its" : "arm,gic-v2m-frame";
    for (int n = fdt_find_compatible(sub, -1); n >= 0 && nframes < 16; n = fdt_find_compatible(sub, n))
        frames[nframes++] = n;

    u32 size = 44 + ncpus * 80 + 24 + (v3 ? 16 : 0) + nframes * (v3 ? 20 : 24);
    u8 *t = acpi_table("APIC", size, 5);
    if (!t)
        return NULL;
    u8 *p = t + 44;
    for (int i = 0; i < ncpus; i++, p += 80) {
        p[0] = 0x0B;                    /* GICC */
        p[1] = 80;
        put32(p + 4, v3 ? 0 : (u32)i);  /* CPU interface number */
        put32(p + 8, (u32)i);           /* ACPI processor UID */
        put32(p + 12, 1);               /* enabled */
        put32(p + 20, pmu);
        if (!v3)
            put64(p + 32, b1);          /* GICC base */
        put64(p + 40, gicv);
        put64(p + 48, gich);
        put32(p + 56, maint);
        put64(p + 68, mpidr[i]);
    }
    p[0] = 0x0C;                        /* GICD */
    p[1] = 24;
    put64(p + 8, gicd);
    p[20] = v3 ? 3 : 2;
    p += 24;
    if (v3) {
        p[0] = 0x0E;                    /* GICR discovery range */
        p[1] = 16;
        put64(p + 4, b1);
        put32(p + 12, (u32)s1);
        p += 16;
    }
    for (int i = 0; i < nframes; i++) {
        u64 base = 0;
        fdt_reg(frames[i], 0, &base, NULL);
        if (v3) {
            p[0] = 0x0F;                /* GIC ITS */
            p[1] = 20;
            put32(p + 4, (u32)i);
            put64(p + 8, base);
            p += 20;
        } else {
            p[0] = 0x0D;                /* GIC MSI frame: SPIs from its TYPER */
            p[1] = 24;
            put32(p + 4, (u32)i);
            put64(p + 8, base);
            p += 24;
        }
    }
    return t;
}

/* ------------------------------------------------------- FADT, GTDT */

static void *fadt(void *dsdt, int smc)
{
    u8 *t = acpi_table("FACP", 276, 6);
    if (!t)
        return NULL;
    put32(t + 112, 1u << 20);           /* HW_REDUCED_ACPI */
    put16(t + 129, smc ? 1 : 3);        /* PSCI_COMPLIANT, PSCI_USE_HVC */
    t[131] = 3;                         /* FADT minor version: ACPI 6.3 */
    put64(t + 140, (uptr)dsdt);         /* X_DSDT */
    return t;
}

static void *gtdt(void)
{
    int n = fdt_find_compatible("arm,armv8-timer", -1);
    u32 len;
    const u8 *ip = n >= 0 ? fdt_prop(n, "interrupts", &len) : NULL;
    if (!ip || len < 48)
        return NULL;
    u8 *t = acpi_table("GTDT", 104, 3);
    if (!t)
        return NULL;
    put64(t + 36, ~0ULL);               /* no CNTControlBase */
    for (int i = 0; i < 4; i++) {       /* secure, non-secure, virtual, EL2 */
        put32(t + 48 + 8 * i, gsi_of(ip + 12 * i));
        put32(t + 52 + 8 * i, timer_flags(ip + 12 * i));
    }
    put64(t + 80, ~0ULL);               /* no CNTReadBase */
    if (len >= 60) {                    /* EL2 virtual timer */
        put32(t + 96, gsi_of(ip + 48));
        put32(t + 100, timer_flags(ip + 48));
    }
    return t;
}

/* ------------------------------------------------------------- SPCR */

static void *spcr(int uart)
{
    u64 base;
    u32 gsi;
    int edge, low;
    if (!fdt_reg(uart, 0, &base, NULL) || !node_irq(uart, &gsi, &edge, &low))
        return NULL;
    u8 *t = acpi_table("SPCR", 80, 2);
    if (!t)
        return NULL;
    t[36] = 0x03;                       /* ARM PL011 */
    struct acpi_gas g = { 0, 32, 0, 3, base };
    memcpy(t + 40, &g, sizeof g);
    t[52] = 0x08;                       /* interrupt: Arm GIC */
    put32(t + 54, gsi);
    t[58] = 7;                          /* 115200 baud */
    t[60] = 1;                          /* one stop bit */
    t[62] = 3;                          /* ANSI */
    put16(t + 64, 0xFFFF);              /* not a PCI device */
    put16(t + 66, 0xFFFF);
    return t;
}

/* Debug port: the same PL011, named \_SB.COM0 (Microsoft's DBG2) */
static void *dbg2(int uart)
{
    static const char ns[] = "\\_SB.COM0";
    u64 base;
    if (!fdt_reg(uart, 0, &base, NULL))
        return NULL;
    u32 info = 22 + 12 + 4 + sizeof ns;
    u8 *t = acpi_table("DBG2", 44 + info, 0);
    if (!t)
        return NULL;
    put32(t + 36, 44);                  /* device information offset */
    put32(t + 40, 1);                   /* one device */
    u8 *d = t + 44;
    put16(d + 1, (u16)info);
    d[3] = 1;                           /* one address register */
    put16(d + 4, sizeof ns);
    put16(d + 6, 22 + 12 + 4);          /* namespace string offset */
    put16(d + 12, 0x8000);              /* serial */
    put16(d + 14, 0x0003);              /* ARM PL011 */
    put16(d + 18, 22);                  /* base address registers */
    put16(d + 20, 22 + 12);             /* address sizes */
    struct acpi_gas g = { 0, 32, 0, 3, base };
    memcpy(d + 22, &g, sizeof g);
    put32(d + 34, 0x1000);
    memcpy(d + 38, ns, sizeof ns);
    return t;
}

/* ------------------------------------------------------- PCIe host */

struct pci_host {
    int node;
    u64 ecam;
    u32 bus_min, bus_max;
};

static int find_pci(struct pci_host *h)
{
    h->node = fdt_find_compatible("pci-host-ecam-generic", -1);
    if (h->node < 0 || !fdt_reg(h->node, 0, &h->ecam, NULL))
        return 0;
    u32 len;
    const u8 *br = fdt_prop(h->node, "bus-range", &len);
    h->bus_min = br && len >= 8 ? be32(br) : 0;
    h->bus_max = br && len >= 8 ? be32(br + 4) : 255;
    return 1;
}

static void *mcfg(const struct pci_host *h)
{
    u8 *t = acpi_table("MCFG", 44 + 16, 1);
    if (!t)
        return NULL;
    put64(t + 44, h->ecam);
    t[54] = (u8)h->bus_min;
    t[55] = (u8)h->bus_max;
    return t;
}

/* PCI0: _CRS from "ranges" (3 child cells, 2 parent, 2 size), _PRT from
 * "interrupt-map" (3 + 1 child cells, phandle, the GIC's address cells,
 * 3 interrupt cells) */
static void dsdt_pci(struct aml *a, const struct pci_host *h, int gic_ac)
{
    aml_device(a, "PCI0");
    aml_name_eisaid(a, "_HID", "PNP0A08");
    aml_name_eisaid(a, "_CID", "PNP0A03");
    aml_name_int(a, "_SEG", 0);
    aml_name_int(a, "_BBN", h->bus_min);
    aml_name_int(a, "_UID", 0);
    aml_name_int(a, "_CCA", 1);

    struct res r = { .len = 0 };
    res_word(&r, 2, (u16)h->bus_min, (u16)h->bus_max);
    u32 len;
    const u8 *p = fdt_prop(h->node, "ranges", &len);
    for (u32 off = 0; p && off + 28 <= len; off += 28) {
        u32 space = be32(p + off) >> 24 & 3;
        u64 pci = be64(p + off + 4), cpu = be64(p + off + 12), size = be64(p + off + 20);
        if (!size)
            continue;
        if (space == 1)
            res_dword_io_translated(&r, (u32)pci, (u32)(pci + size - 1), (u32)cpu);
        else if (space == 2 && pci == cpu)
            res_dword_mem(&r, (u32)cpu, (u32)(cpu + size - 1), 0);
        else if (space == 3 && pci == cpu)
            res_qword_mem(&r, cpu, cpu + size - 1, 0);
    }
    aml_name_resources(a, "_CRS", &r);

    const u8 *m = fdt_prop(h->node, "interrupt-map", &len);
    u32 stride = (3 + 1 + 1 + gic_ac + 3) * 4;
    u32 count = m ? len / stride : 0;
    if (count > 255)
        count = 255;
    aml_byte(a, 0x08);                  /* Name(_PRT, Package() {...}) */
    aml_name(a, "_PRT");
    aml_package(a, (u8)count);
    for (u32 i = 0; i < count; i++) {
        const u8 *e = m + i * stride;
        u32 dev = be32(e) >> 11 & 0x1F;
        u32 pin = be32(e + 12) - 1;
        u32 gsi = gsi_of(e + (3 + 1 + 1 + gic_ac) * 4);
        aml_package(a, 4);
        aml_int(a, (u64)dev << 16 | 0xFFFF);
        aml_int(a, pin);
        aml_int(a, 0);
        aml_int(a, gsi);
        aml_end(a);
    }
    aml_end(a);

    /* The ECAM window, claimed as a motherboard resource so an OS can
     * see it is reserved */
    aml_device(a, "RES0");
    aml_name_eisaid(a, "_HID", "PNP0C02");
    struct res e = { .len = 0 };
    u64 ecam_size = (u64)(h->bus_max - h->bus_min + 1) << 20;
    res_qword_mem(&e, h->ecam, h->ecam + ecam_size - 1, 0);
    e.buf[4] |= 0x01;                   /* consumer */
    aml_name_resources(a, "_CRS", &e);
    aml_end(a);
    aml_end(a);
}

/* ------------------------------------------------------------- IORT */

/* With a GICv3 ITS, PCI MSIs need the IO Remapping Table (Arm DEN 0049,
 * table revision 0): one ITS group holding every ITS, and the root
 * complex mapping requester IDs 0-0xFFFF one to one onto ITS device IDs,
 * as the host bridge's msi-map says */
static void *iort(int nits)
{
    u32 its_len = 16 + 4 + 4 * nits;
    u32 rc_len = 32 + 20;
    u8 *t = acpi_table("IORT", 48 + its_len + rc_len, 0);
    if (!t)
        return NULL;
    put32(t + 36, 2);                   /* nodes */
    put32(t + 40, 48);                  /* where they start */
    u8 *n = t + 48;
    n[0] = 0;                           /* ITS group */
    put16(n + 1, (u16)its_len);
    put32(n + 16, (u32)nits);
    for (int i = 0; i < nits; i++)
        put32(n + 20 + 4 * i, (u32)i);  /* the MADT's GIC ITS IDs */
    n += its_len;
    n[0] = 2;                           /* PCI root complex */
    put16(n + 1, (u16)rc_len);
    put32(n + 8, 1);                    /* one ID mapping */
    put32(n + 12, 32);                  /* at the end of the node */
    put32(n + 16, 1);                   /* cache coherent */
    n[23] = 3;                          /* CPM, DACS */
    u8 *m = n + 32;
    put32(m + 4, 0xFFFF);               /* 0x10000 IDs */
    put32(m + 12, 48);                  /* to the ITS group */
    return t;
}

/* ------------------------------------------------------------- DSDT */

static void dsdt_mmio_device(struct aml *a, const char *name, const char *hid, u32 uid, int node,
                             u32 min_size, int cca)
{
    u64 base, size;
    if (!fdt_reg(node, 0, &base, &size))
        return;
    aml_device(a, name);
    aml_name_string(a, "_HID", hid);
    aml_name_int(a, "_UID", uid);
    if (cca)
        aml_name_int(a, "_CCA", 1);
    struct res r = { .len = 0 };
    res_mem32_fixed(&r, (u32)base, (u32)(size > min_size ? size : min_size), 1);
    u32 gsi;
    int edge, low;
    if (node_irq(node, &gsi, &edge, &low))
        res_interrupt(&r, gsi, edge, low);
    aml_name_resources(a, "_CRS", &r);
    aml_end(a);
}

static void *dsdt(int uart, const struct pci_host *pci, int gic_ac)
{
    static u8 buf[16384];
    struct aml a;
    aml_init(&a, buf, sizeof buf);
    aml_scope(&a, "\\_SB");
    for (int i = 0; i < ncpus; i++) {
        char name[5] = { 'C', "0123456789ABCDEF"[i >> 8 & 0xF], "0123456789ABCDEF"[i >> 4 & 0xF],
                         "0123456789ABCDEF"[i & 0xF], 0 };
        aml_device(&a, name);
        aml_name_string(&a, "_HID", "ACPI0007");
        aml_name_int(&a, "_UID", (u64)i);
        aml_end(&a);
    }
    if (uart >= 0)
        dsdt_mmio_device(&a, "COM0", "ARMH0011", 0, uart, 0x1000, 0);
    int fw = fdt_find_compatible("qemu,fw-cfg-mmio", -1);
    if (fw >= 0)
        dsdt_mmio_device(&a, "FWCF", "QEMU0002", 0, fw, 0x18, 1);
    u32 nv = 0;
    for (int n = fdt_find_compatible("virtio,mmio", -1); n >= 0 && nv < 256;
         n = fdt_find_compatible("virtio,mmio", n), nv++) {
        char name[5] = { 'V', 'R', "0123456789ABCDEF"[nv >> 4], "0123456789ABCDEF"[nv & 0xF], 0 };
        dsdt_mmio_device(&a, name, "LNRO0005", nv, n, 0x200, 1);
    }
    if (pci)
        dsdt_pci(&a, pci, gic_ac);
    aml_end(&a);
    if (a.overflow || a.depth)
        return NULL;

    u8 *t = acpi_table("DSDT", 36 + a.len, 2);
    if (!t)
        return NULL;
    memcpy(t + 36, buf, a.len);
    acpi_checksum_table(t);
    return t;
}

/* ------------------------------------------------------------------ */

void arm_acpi_init(void)
{
    find_cpus();
    int v3 = 1;
    int gic = fdt_find_compatible("arm,gic-v3", -1);
    if (gic < 0) {
        v3 = 0;
        gic = fdt_find_compatible("arm,cortex-a15-gic", -1);
        if (gic < 0)
            gic = fdt_find_compatible("arm,gic-400", -1);
    }
    int psci = fdt_find_compatible("arm,psci-0.2", -1);
    const char *method = psci >= 0 ? fdt_prop(psci, "method", NULL) : NULL;
    if (!ncpus || gic < 0 || !method) {
        printf("acpi: no tables (needs CPUs, a GIC and PSCI in the device tree)\n");
        return;
    }
    u32 len;
    const u8 *ac = fdt_prop(gic, "#address-cells", &len);
    int gic_ac = ac && len == 4 ? (int)be32(ac) : 0;
    int uart = fdt_find_compatible("arm,pl011", -1);
    struct pci_host pci;
    int have_pci = find_pci(&pci);

    if (!acpi_begin(16)) {
        printf("acpi: no memory for the tables\n");
        return;
    }
    void *d = dsdt(uart, have_pci ? &pci : NULL, gic_ac);
    void *f = d ? fadt(d, !strcmp(method, "smc")) : NULL;
    void *m = madt(gic, v3);
    void *g = gtdt();
    if (!f || !m || !g) {
        printf("acpi: building the tables failed\n");
        return;
    }
    acpi_add(f);
    acpi_add(m);
    acpi_add(g);
    void *s = uart >= 0 ? spcr(uart) : NULL;
    acpi_add(s);
    acpi_add(s ? dbg2(uart) : NULL);
    acpi_add(have_pci ? mcfg(&pci) : NULL);
    int nits = 0;
    for (int n = fdt_find_compatible("arm,gic-v3-its", -1); n >= 0 && nits < 16;
         n = fdt_find_compatible("arm,gic-v3-its", n))
        nits++;
    void *io = v3 && nits && have_pci ? iort(nits) : NULL;
    acpi_add(io);
    void *rsdp = acpi_publish();
    if (!rsdp) {
        printf("acpi: publishing the tables failed\n");
        return;
    }
    printf("acpi: RSDP at %p: FACP APIC GTDT%s%s%s, DSDT of %u bytes; %d CPUs, GICv%d\n", rsdp,
           s ? " SPCR DBG2" : "", have_pci ? " MCFG" : "", io ? " IORT" : "", ((struct acpi_header *)d)->length, ncpus,
           v3 ? 3 : 2);
}
