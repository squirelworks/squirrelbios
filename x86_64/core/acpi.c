/* SquirrelBIOS x86_64-uefi: ACPI 6.x tables (roadmap C2).
 * Original code, written from the ACPI 6.5 specification (sections 5.2
 * and 6) and the QEMU machine's documented hardware: PIIX4 or ICH9 PM
 * block, IOAPIC at FEC00000h, HPET at FED00000h, local APICs at
 * FEE00000h, PCI Express ECAM on q35 (chipset.c).
 *
 * Tables: RSDP 2.0, XSDT and RSDT, FADT 6.4, FACS, DSDT (AML generated
 * at boot by aml.c), MADT (from the SMP list: local APIC entries, x2APIC
 * ones past ID 254), HPET (when the timer answers), MCFG (q35), and from
 * x86_64/acpi/: SRAT and SLIT with QEMU -numa, BERT and HEST.
 * They live in ACPI reclaim memory (FACS in ACPI NVS) and the RSDP is
 * published as the ACPI 2.0 and 1.0 configuration tables. */
#include "acpi.h"
#include "smp.h"
#include "tpm.h"

static const EFI_GUID acpi20_guid =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const EFI_GUID acpi10_guid =
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

#define LAPIC_BASE  0xFEE00000u
#define IOAPIC_BASE 0xFEC00000u
#define HPET_BASE   0xFED00000u

#define TABLE_PAGES 32
#define XROOT_MAX      6                /* extra root buses with windows */
#define XROOT_IO       0x6000           /* their I/O windows, 4 KB each */
#define XROOT_MEM      0xE0000000u      /* their memory windows */
#define XROOT_MEM_SIZE 0x04000000u

u8 acpi_checksum(const void *p, u32 len)
{
    const u8 *b = p;
    u8 sum = 0;
    while (len--)
        sum = (u8)(sum + *b++);
    return (u8)-sum;
}

/* A bump allocator inside the reclaim block, 16-byte aligned */
static u8 *area, *area_end, *next;

static void *take(u32 len)
{
    u8 *p = next;
    next += (len + 15) & ~15u;
    if (next > area_end)
        return NULL;
    memset(p, 0, len);
    return p;
}

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

void acpi_header(u8 *t, const char *sig, u32 len, u8 rev)
{
    memcpy(t, sig, 4);
    put32(t + 4, len);
    t[8] = rev;
    memcpy(t + 10, ACPI_OEM_ID, 6);
    memcpy(t + 16, ACPI_OEM_TABLE, 8);
    put32(t + 24, 1);                   /* OEM revision */
    memcpy(t + 28, "SQRL", 4);          /* creator */
    put32(t + 32, 1);
}

void acpi_seal(u8 *t)
{
    u32 len = t[4] | (u32)t[5] << 8 | (u32)t[6] << 16 | (u32)t[7] << 24;
    t[9] = 0;
    t[9] = acpi_checksum(t, len);
}

/* Generic Address Structure */
static void gas(u8 *g, u8 space, u8 width, u8 access, u64 addr)
{
    g[0] = space;
    g[1] = width;
    g[2] = 0;
    g[3] = access;
    put64(g + 4, addr);
}


static int hpet_present(void)
{
    volatile u32 *h = (volatile u32 *)(uptr)HPET_BASE;
    u32 cap = h[0];
    return cap != 0xFFFFFFFFu && cap != 0 && (cap >> 16) != 0;
}

/* Highest end of usable RAM below 4 GB: where the PCI hole starts. */
static u32 low_ram_top(void)
{
    u64 top = 0x100000;
    for (int i = 0; i < mem_count; i++) {
        u64 end = mem_map[i].base + mem_map[i].length;
        if (mem_map[i].type == 1 && end <= 0x100000000ull && end > top)
            top = end;
    }
    return (u32)top;
}

/* DSDT */

static void link_device(struct aml *a, const char *name, u32 uid, u32 gsi)
{
    struct res r = { .len = 0 };
    res_interrupt_level_high(&r, gsi);
    aml_device(a, name);
    aml_name_eisaid(a, "_HID", "PNP0C0F");
    aml_name_int(a, "_UID", uid);
    aml_name_int(a, "_STA", 0x0B);      /* present, enabled, functional */
    aml_name_resources(a, "_PRS", &r);
    aml_name_resources(a, "_CRS", &r);
    /* The routing is fixed by chipset.c: accept any setting, keep it. */
    aml_method(a, "_SRS", 1);
    aml_end(a);
    aml_method(a, "_DIS", 0);
    aml_end(a);
    aml_end(a);
}

static void legacy_device(struct aml *a, const char *name, const char *hid, u32 uid,
                          u16 io1, u8 len1, u16 io2, u8 len2, int irq)
{
    struct res r = { .len = 0 };
    if (len1)
        res_io(&r, io1, len1);
    if (len2)
        res_io(&r, io2, len2);
    if (irq >= 0)
        res_irq(&r, (u8)irq);
    aml_device(a, name);
    aml_name_eisaid(a, "_HID", hid);
    aml_name_int(a, "_UID", uid);
    aml_name_int(a, "_STA", 0x0F);
    aml_name_resources(a, "_CRS", &r);
    aml_end(a);
}

static u32 build_dsdt(u8 *t, u32 cap, int hpet)
{
    const struct chipset *cs = chipset();
    struct aml a;
    aml_init(&a, t + 36, cap - 36);

    aml_scope(&a, "\\_SB");

    /* Extra root buses (pxb, pxb-pcie): each gets the buses up to the
     * next root, a 4 KB I/O window and a 64 MB memory window of its
     * own; PCI0 keeps the rest */
    u8 roots[XROOT_MAX];
    int nroots = pci_extra_roots(roots, XROOT_MAX);

    /* Host bridge and its windows */
    aml_device(&a, "PCI0");
    if (cs->q35) {
        aml_name_eisaid(&a, "_HID", "PNP0A08");
        aml_name_eisaid(&a, "_CID", "PNP0A03");
    } else {
        aml_name_eisaid(&a, "_HID", "PNP0A03");
    }
    aml_name_int(&a, "_UID", 0);
    aml_name_int(&a, "_SEG", 0);
    aml_name_int(&a, "_BBN", 0);
    {
        struct res r = { .len = 0 };
        u32 win = low_ram_top();
        if (cs->q35 && cs->mcfg_base && win < cs->mcfg_base + (cs->mcfg_buses << 20))
            win = (u32)(cs->mcfg_base + (cs->mcfg_buses << 20));
        win = (win + 0xFFFFF) & ~0xFFFFFu;
        res_word(&r, 2, 0, nroots ? (u16)(roots[0] - 1) : 0xFF);
        res_io(&r, 0xCF8, 8);
        res_word(&r, 1, 0x0000, 0x0CF7);
        if (nroots) {
            res_word(&r, 1, 0x0D00, XROOT_IO - 1);
            res_word(&r, 1, (u16)(XROOT_IO + nroots * 0x1000), 0xFFFF);
        } else {
            res_word(&r, 1, 0x0D00, 0xFFFF);
        }
        res_dword_mem(&r, 0x000A0000, 0x000BFFFF, 1);
        if (nroots && win < XROOT_MEM) {
            res_dword_mem(&r, win, XROOT_MEM - 1, 0);
            res_dword_mem(&r, XROOT_MEM + (u32)nroots * XROOT_MEM_SIZE, 0xFEBFFFFF, 0);
        } else {
            res_dword_mem(&r, win, 0xFEBFFFFF, 0);
        }
        aml_name_resources(&a, "_CRS", &r);
    }
    /* Interrupt routing for every bus 0 slot and pin, through the link
     * devices below. */
    aml_byte(&a, 0x08);
    aml_name(&a, "_PRT");
    aml_package(&a, 128);
    for (u8 slot = 0; slot < 32; slot++) {
        for (u8 pin = 0; pin < 4; pin++) {
            u8 pirq = chipset_pirq(slot, pin);
            char link[] = "\\_SB.LNKA";
            if (cs->q35) {
                memcpy(link + 5, "GSI", 3);
                link[8] = (char)('A' + pirq);
            } else {
                link[8] = pirq == 8 ? 'S' : (char)('A' + pirq);
            }
            aml_package(&a, 4);
            aml_int(&a, (u32)slot << 16 | 0xFFFF);
            aml_int(&a, pin);
            aml_name(&a, link);
            aml_int(&a, 0);
            aml_end(&a);
        }
    }
    aml_end(&a);                        /* _PRT */
    aml_end(&a);                        /* PCI0 */

    for (int k = 0; k < nroots; k++) {
        static const char hex[] = "0123456789ABCDEF";
        char name[] = "PC00";
        name[2] = hex[roots[k] >> 4];
        name[3] = hex[roots[k] & 15];
        aml_device(&a, name);
        if (cs->q35) {
            aml_name_eisaid(&a, "_HID", "PNP0A08");
            aml_name_eisaid(&a, "_CID", "PNP0A03");
        } else {
            aml_name_eisaid(&a, "_HID", "PNP0A03");
        }
        aml_name_int(&a, "_UID", roots[k]);
        aml_name_int(&a, "_SEG", 0);
        aml_name_int(&a, "_BBN", roots[k]);
        int pxm = acpi_qemu_bus_pxm(roots[k]);
        if (pxm >= 0)
            aml_name_int(&a, "_PXM", (u32)pxm);
        struct res r = { .len = 0 };
        res_word(&r, 2, roots[k], k + 1 < nroots ? (u16)(roots[k + 1] - 1) : 0xFF);
        u32 win = low_ram_top();
        if (win < XROOT_MEM) {
            u16 io = (u16)(XROOT_IO + k * 0x1000);
            u32 mem = XROOT_MEM + (u32)k * XROOT_MEM_SIZE;
            res_word(&r, 1, io, (u16)(io + 0xFFF));
            res_dword_mem(&r, mem, mem + XROOT_MEM_SIZE - 1, 0);
        }
        aml_name_resources(&a, "_CRS", &r);
        if (!cs->q35) {
            /* QEMU's pxb takes its own slot back out of the swizzle,
             * so a device's pin lands on link (slot + pin - 1) & 3 */
            aml_byte(&a, 0x08);
            aml_name(&a, "_PRT");
            aml_package(&a, 128);
            for (u8 slot = 0; slot < 32; slot++)
                for (u8 pin = 0; pin < 4; pin++) {
                    char link[] = "\\_SB.LNKA";
                    link[8] = (char)('A' + ((slot + pin + 3) & 3));
                    aml_package(&a, 4);
                    aml_int(&a, (u32)slot << 16 | 0xFFFF);
                    aml_int(&a, pin);
                    aml_name(&a, link);
                    aml_int(&a, 0);
                    aml_end(&a);
                }
            aml_end(&a);                /* _PRT */
        }
        aml_end(&a);                    /* PCxx */
    }
    for (int k = 0; k < nroots; k++) {
        u32 last = k + 1 < nroots ? roots[k + 1] - 1u : 0xFFu;
        int pxm = acpi_qemu_bus_pxm(roots[k]);
        printf("pci: root bus %02x-%02x (PC%02X)", roots[k], last, roots[k]);
        if (pxm >= 0)
            printf(", node %d", pxm);
        if (low_ram_top() < XROOT_MEM)
            printf(", I/O %04x-%04x, memory %08x-%08x", XROOT_IO + k * 0x1000, XROOT_IO + k * 0x1000 + 0xFFF,
                   XROOT_MEM + (u32)k * XROOT_MEM_SIZE, XROOT_MEM + (u32)(k + 1) * XROOT_MEM_SIZE - 1);
        printf("\n");
    }

    if (cs->q35) {
        char name[] = "GSIA";
        for (u8 i = 0; i < 8; i++) {
            name[3] = (char)('A' + i);
            link_device(&a, name, 0x10 + i, chipset_pirq_irq(i));
        }
    } else {
        char name[] = "LNKA";
        for (u8 i = 0; i < 4; i++) {
            name[3] = (char)('A' + i);
            link_device(&a, name, i, chipset_pirq_irq(i));
        }
        link_device(&a, "LNKS", 4, 9);
    }

    /* ISA devices QEMU always has */
    legacy_device(&a, "COM1", "PNP0501", 1, 0x3F8, 8, 0, 0, 4);
    legacy_device(&a, "RTC", "PNP0B00", 0, 0x70, 2, 0, 0, 8);
    legacy_device(&a, "KBD", "PNP0303", 0, 0x60, 1, 0x64, 1, 1);
    legacy_device(&a, "MOU", "PNP0F13", 0, 0, 0, 0, 0, 12);
    legacy_device(&a, "TMR", "PNP0100", 0, 0x40, 4, 0, 0, 0);
    if (hpet) {
        struct res r = { .len = 0 };
        res_mem32_fixed(&r, HPET_BASE, 0x400, 0);
        aml_device(&a, "HPET");
        aml_name_eisaid(&a, "_HID", "PNP0103");
        aml_name_int(&a, "_UID", 0);
        aml_name_int(&a, "_STA", 0x0F);
        aml_name_resources(&a, "_CRS", &r);
        aml_end(&a);
    }
    if (tpm_present()) {
        struct res r = { .len = 0 };
        res_mem32_fixed(&r, TPM_TIS_BASE, TPM_TIS_SIZE, 1);
        aml_device(&a, "TPM");
        aml_name_string(&a, "_HID", "MSFT0101");
        aml_name_string(&a, "_CID", "MSFT0101");
        aml_name_int(&a, "_STA", 0x0F);
        aml_name_resources(&a, "_CRS", &r);
        aml_end(&a);
    }
    /* Motherboard resources: the PM block and, on q35, the ECAM window,
     * so the OS knows both are taken. */
    {
        struct res r = { .len = 0 };
        res_io(&r, cs->pm_base, 0x80);
        if (cs->q35 && cs->mcfg_base)
            res_mem32_fixed(&r, (u32)cs->mcfg_base, cs->mcfg_buses << 20, 0);
        aml_device(&a, "MRES");
        aml_name_eisaid(&a, "_HID", "PNP0C02");
        aml_name_int(&a, "_UID", 1);
        aml_name_resources(&a, "_CRS", &r);
        aml_end(&a);
    }
    aml_end(&a);                        /* \_SB */

    /* \_S5: soft off */
    aml_byte(&a, 0x08);
    aml_name(&a, "\\_S5");
    aml_package(&a, 4);
    aml_int(&a, ACPI_S5_SLP_TYP);
    aml_int(&a, ACPI_S5_SLP_TYP);
    aml_int(&a, 0);
    aml_int(&a, 0);
    aml_end(&a);
    /* \_PIC: the routing is the same in either mode */
    aml_method(&a, "\\_PIC", 1);
    aml_end(&a);

    if (a.overflow || a.depth)
        return 0;
    u32 len = 36 + a.len;
    acpi_header(t, "DSDT", len, 2);
    acpi_seal(t);
    return len;
}

/* One entry per processor smp.c brought up, in processor-number order,
 * the UID being that number: local APIC entries for APIC IDs up to 254,
 * x2APIC entries (and the x2APIC NMI) past that */
static u32 build_madt(u8 *t, u32 cpus)
{
    u8 *p = t + 44;
    int x2 = 0;
    put32(t + 36, LAPIC_BASE);
    put32(t + 40, 1);                   /* PCAT_COMPAT: 8259s present */
    for (u32 i = 0; i < cpus; i++) {
        const struct smp_cpu_info *ci = smp_cpu((int)i);
        u32 id = ci ? ci->apic_id : i;
        u32 on = ci ? (ci->enabled != 0) : 1;
        if (id < 255 && i < 255) {
            p[0] = 0;                   /* processor local APIC */
            p[1] = 8;
            p[2] = (u8)i;
            p[3] = (u8)id;
            put32(p + 4, on);
            p += 8;
        } else {
            p[0] = 9;                   /* processor local x2APIC */
            p[1] = 16;
            put16(p + 2, 0);
            put32(p + 4, id);
            put32(p + 8, on);
            put32(p + 12, i);
            p += 16;
            x2 = 1;
        }
    }
    volatile u32 *io = (volatile u32 *)(uptr)IOAPIC_BASE;
    io[0] = 0;                          /* IOAPICID register */
    u8 ioapic_id = (u8)((io[4] >> 24) & 0x0F);
    p[0] = 1;
    p[1] = 12;
    p[2] = ioapic_id;
    p[3] = 0;
    put32(p + 4, IOAPIC_BASE);
    put32(p + 8, 0);                    /* GSI base */
    p += 12;
    /* ISA IRQ 0 is pin 2; the SCI and the PCI link IRQs are level,
     * active high, as QEMU wires them. */
    static const u8 iso_src[] = { 0, 5, 9, 10, 11 };
    for (u32 i = 0; i < sizeof iso_src; i++) {
        p[0] = 2;
        p[1] = 10;
        p[2] = 0;
        p[3] = iso_src[i];
        put32(p + 4, iso_src[i] == 0 ? 2 : iso_src[i]);
        put16(p + 8, iso_src[i] == 0 ? 0 : 0x000D);
        p += 10;
    }
    p[0] = 4;                           /* local APIC NMI on LINT1, all CPUs */
    p[1] = 6;
    p[2] = 0xFF;
    put16(p + 3, 0);
    p[5] = 1;
    p += 6;
    if (x2) {
        p[0] = 0x0A;                    /* local x2APIC NMI on LINT1, all CPUs */
        p[1] = 12;
        put16(p + 2, 0);
        put32(p + 4, 0xFFFFFFFFu);
        p[8] = 1;
        p += 12;
    }
    u32 len = (u32)(p - t);
    acpi_header(t, "APIC", len, 5);
    acpi_seal(t);
    return len;
}

static void build_fadt(u8 *t, u32 facs, u32 dsdt)
{
    const struct chipset *cs = chipset();
    u16 pm = cs->pm_base;
    acpi_header(t, "FACP", 276, 6);
    put32(t + 36, facs);
    put32(t + 40, dsdt);
    t[45] = 0;                          /* preferred PM profile: unspecified */
    put16(t + 46, 9);                   /* SCI on ISA IRQ 9 */
    put32(t + 48, 0);                   /* no SMI command port: already ACPI */
    put32(t + 56, pm);                  /* PM1a event */
    put32(t + 64, pm + 4u);             /* PM1a control */
    put32(t + 76, pm + 8u);             /* PM timer */
    put32(t + 80, cs->gpe0);
    t[88] = 4;
    t[89] = 2;
    t[91] = 4;
    t[92] = cs->gpe0_len;
    put16(t + 96, 0x0FFF);              /* no C2 */
    put16(t + 98, 0x0FFF);              /* no C3 */
    t[108] = 0x32;                      /* RTC century register */
    put16(t + 109, 0x0003);             /* legacy devices, 8042 */
    /* WBINVD, PROC_C1, SLP_BUTTON (none), RTC_S4, RESET_REG_SUP */
    put32(t + 112, (1u << 0) | (1u << 2) | (1u << 5) | (1u << 7) | (1u << 10));
    gas(t + 116, 1, 8, 1, 0xCF9);       /* reset register */
    t[128] = 0x06;
    t[131] = 4;                         /* FADT minor version: 6.4 */
    put64(t + 140, dsdt);               /* X_DSDT */
    gas(t + 148, 1, 32, 2, pm);
    gas(t + 172, 1, 16, 2, pm + 4u);
    gas(t + 208, 1, 32, 3, pm + 8u);
    gas(t + 220, 1, (u8)(cs->gpe0_len * 8), 1, cs->gpe0);
    memcpy(t + 268, "SquirrlB", 8);     /* hypervisor vendor identity */
    acpi_seal(t);
}

static u32 build_hpet(u8 *t)
{
    volatile u32 *h = (volatile u32 *)(uptr)HPET_BASE;
    acpi_header(t, "HPET", 56, 1);
    put32(t + 36, h[0]);                /* event timer block ID */
    gas(t + 40, 0, 64, 0, HPET_BASE);
    t[52] = 0;                          /* HPET number */
    put16(t + 53, 0x0080);              /* minimum tick in periodic mode */
    t[55] = 0;
    acpi_seal(t);
    return 56;
}

static u32 build_mcfg(u8 *t)
{
    const struct chipset *cs = chipset();
    acpi_header(t, "MCFG", 60, 1);
    put64(t + 44, cs->mcfg_base);
    put16(t + 52, 0);                   /* segment */
    t[54] = 0;
    t[55] = (u8)(cs->mcfg_buses - 1);
    acpi_seal(t);
    return 60;
}

void acpi_init(void)
{
    /* The fixed tables, plus room that grows with the CPUs: a MADT
     * x2APIC entry (16 bytes) and an SRAT x2APIC affinity entry (24)
     * each, so a thousand CPUs and more fit */
    EFI_PHYSICAL_ADDRESS base = 0, facs_page = 0;
    u32 pages = TABLE_PAGES + ((u32)smp_cpu_count() * 48 + 4095) / 4096;
    if (efi_allocate_pages(AllocateAnyPages, EfiACPIReclaimMemory, pages, &base) ||
        efi_allocate_pages(AllocateAnyPages, EfiACPIMemoryNVS, 1, &facs_page)) {
        printf("acpi: no memory for the tables\n");
        return;
    }
    area = next = (u8 *)(uptr)base;
    area_end = area + pages * 4096;
    const struct chipset *cs = chipset();
    int hpet = hpet_present();
    u32 cpus = (u32)smp_cpu_count();
    if (!cpus)
        cpus = 1;

    /* FACS: 64 bytes, 64-byte aligned, in NVS */
    u8 *facs = (u8 *)(uptr)facs_page;
    memset(facs, 0, 64);
    memcpy(facs, "FACS", 4);
    put32(facs + 4, 64);
    facs[32] = 2;                       /* version */

    u8 *dsdt = area;
    u32 dsdt_len = build_dsdt(dsdt, TABLE_PAGES * 4096 / 2, hpet);
    if (!dsdt_len) {
        printf("acpi: DSDT does not fit\n");
        return;
    }
    next = area + ((dsdt_len + 15) & ~15u);

    u8 *tables[16];
    int n = 0;
    u8 *fadt = take(276);
    build_fadt(fadt, (u32)(uptr)facs, (u32)(uptr)dsdt);
    tables[n++] = fadt;
    u8 *madt = take(44 + cpus * 16 + 12 + 5 * 10 + 6 + 12);
    build_madt(madt, cpus);
    tables[n++] = madt;
    if (hpet) {
        u8 *h = take(56);
        build_hpet(h);
        tables[n++] = h;
    }
    if (cs->q35 && cs->mcfg_base) {
        u8 *m = take(60);
        build_mcfg(m);
        tables[n++] = m;
    }
    n += acpi_numa_tables(tables + n, take);
    n += acpi_ras_tables(tables + n, take);
    n += tpm_acpi_tables(tables + n, take);

    u8 *xsdt = take(36 + 8 * (u32)n);
    u8 *rsdt = take(36 + 4 * (u32)n);
    u8 *rsdp = take(36);
    if (!rsdp) {
        printf("acpi: tables do not fit\n");
        return;
    }
    acpi_header(xsdt, "XSDT", 36 + 8 * (u32)n, 1);
    acpi_header(rsdt, "RSDT", 36 + 4 * (u32)n, 1);
    for (int i = 0; i < n; i++) {
        put64(xsdt + 36 + 8 * i, (u64)(uptr)tables[i]);
        put32(rsdt + 36 + 4 * i, (u32)(uptr)tables[i]);
    }
    acpi_seal(xsdt);
    acpi_seal(rsdt);

    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, ACPI_OEM_ID, 6);
    rsdp[15] = 2;
    put32(rsdp + 16, (u32)(uptr)rsdt);
    put32(rsdp + 20, 36);
    put64(rsdp + 24, (u64)(uptr)xsdt);
    rsdp[8] = acpi_checksum(rsdp, 20);
    rsdp[32] = acpi_checksum(rsdp, 36);

    efi_install_configuration_table((EFI_GUID *)&acpi20_guid, rsdp);
    efi_install_configuration_table((EFI_GUID *)&acpi10_guid, rsdp);

    printf("acpi: RSDP at %p, XSDT at %p, %u CPU%s, %s, PM at 0x%x\n", rsdp, xsdt, cpus,
           cpus == 1 ? "" : "s", cs->q35 ? "q35 (ICH9)" : "i440fx (PIIX4)", cs->pm_base);
    printf("acpi: FACP APIC%s%s", hpet ? " HPET" : "", cs->q35 && cs->mcfg_base ? " MCFG" : "");
    for (int i = 0; i < n; i++)
        if (!memcmp(tables[i], "SRAT", 4) || !memcmp(tables[i], "SLIT", 4) ||
            !memcmp(tables[i], "HMAT", 4) || !memcmp(tables[i], "BERT", 4) ||
            !memcmp(tables[i], "HEST", 4) || !memcmp(tables[i], "EINJ", 4) ||
            !memcmp(tables[i], "TPM2", 4))
            printf(" %c%c%c%c", tables[i][0], tables[i][1], tables[i][2], tables[i][3]);
    printf(" DSDT (%u bytes of AML) FACS\n", dsdt_len - 36);
}
