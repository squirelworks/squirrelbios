/* SquirrelBIOS sparc64-sun4u: firmware for QEMU's sun4u machine.
 * Console, fw_cfg, the device tree, an IEEE 1275-style client interface
 * (no Forth), the TLB mappings for the OS, the kernel hand-off and the
 * sq> prompt.
 * Original code, written from IEEE Std 1275-1994 and its SPARC (64-bit)
 * binding for the client interface, and the UltraSPARC-IIi manual.
 */
#include "fwlib.h"

#ifdef SQ_CONFIG
#include "config.h"
#endif
#ifndef CONFIG_IDENTITY_MB
#define CONFIG_IDENTITY_MB 32       /* RAM mapped 1:1 for the OS, in 4 MB pages */
#endif

#define SQ_VERSION   "3.0.0-alpha"
#define FW_VBASE     ((uint64_t)0xf0000000)
#define FW_SIZE      ((uint64_t)0x400000)
#define FOUR_MB      ((uint64_t)0x400000)
#define IO_BASE      0x1fe02000000ull   /* the PCI I/O space, via the PBM */
#define UART         (IO_BASE + 0x3f8)
#define FWCFG        (IO_BASE + 0x510)
#define PBM_REGS     0x1fe00000000ull   /* the Sabre (PBM) controller registers */
#define PBM_CONFIG   0x1fe01000000ull   /* PCI configuration space */
#define PBM_MEM      0x1ff00000000ull   /* PCI memory space */
#define PBM_PORTID   0x1f               /* its UPA port id, the interrupt group */
#define PBM_OBIO_IMAP(ino) (PBM_REGS + 0x1000 + (((ino) & 0x1f) << 3))
#define PBM_OBIO_ICLR(ino) (PBM_REGS + 0x1800 + (((ino) & 0x1f) << 3))
#define INO_SERIAL   0x2b               /* the EBus serial line (ISA IRQ 4) */
#define INO_PBM_UE   0x2e               /* the PBM's own error interrupts */
#define INO_PBM_CE   0x2f
#define INO_PBM_ERR  0x30
#define SIMBA_A_DEVFN 0x09              /* 00:01.1, the bus with the EBus */
#define SIMBA_B_DEVFN 0x08              /* 00:01.0 */
#define EBUS_DEVFN   0x08               /* 01:01.0 */
#define INO_IDE      0x20               /* QEMU wires the on-board IDE and NIC */
#define INO_NIC      0x21               /* to fixed OBIO interrupts */
#define NVRAM_IO     (IO_BASE + 0x2000) /* the M48T59, on the EBus */
#define IDPROM_OFF   0x1fd8             /* its ID PROM: format, machine, MAC, ... */
#define SU_PATH      "/pci@1fe,0/pci@1,1/ebus@1/su@14,3f8"
#define EM_SPARCV9   43

#define ASI_LSU_CONTROL         0x45
#define ASI_IMMU                0x50
#define ASI_ITLB_DATA_IN        0x54
#define ASI_ITLB_DATA_ACCESS    0x55
#define ASI_ITLB_TAG_READ       0x56
#define ASI_IMMU_DEMAP          0x57
#define ASI_DMMU                0x58
#define ASI_DTLB_DATA_IN        0x5c
#define ASI_DTLB_DATA_ACCESS    0x5d
#define ASI_DTLB_TAG_READ       0x5e
#define ASI_DMMU_DEMAP          0x5f

#define TTE_V    (1ull << 63)
#define TTE_4M   (3ull << 61)
#define TTE_L    (1ull << 6)
#define TTE_CP   (1ull << 5)
#define TTE_CV   (1ull << 4)
#define TTE_P    (1ull << 2)
#define TTE_W    (1ull << 1)
#define TTE_G    1ull
#define TTE_PA_MASK 0x000001ffffffe000ull

void fw_enter(uint64_t entry, uint64_t cif, uint64_t o0, uint64_t o1) __attribute__((noreturn));
void fw_set_tba(uint64_t tba);
extern char trap_table[];
int64_t cif_handler(uint64_t *args);
int64_t cif_entry(uint64_t *args);    /* start.S: keeps the client's globals */

static uint64_t fw_pbase, ram_size, tick_hz = 100000000;
static int ncpus = 1;

/* --- I/O through the physical bypass ASIs ---------------------------------- */

static inline uint8_t in8(uint64_t a)
{
    uint8_t v;
    __asm__ volatile("lduba [%1] 0x15, %0" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline void out8(uint64_t a, uint8_t v)
{
    __asm__ volatile("stba %0, [%1] 0x15" : : "r"(v), "r"(a) : "memory");
}

static inline void out16le(uint64_t a, uint16_t v)
{
    __asm__ volatile("stha %0, [%1] 0x1d" : : "r"(v), "r"(a) : "memory");
}

static inline uint64_t in64(uint64_t a)
{
    uint64_t v;
    __asm__ volatile("ldxa [%1] 0x15, %0" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline void out64(uint64_t a, uint64_t v)
{
    __asm__ volatile("stxa %0, [%1] 0x15" : : "r"(v), "r"(a) : "memory");
}

/* PCI configuration space, little-endian through the PBM */
static inline uint64_t cfg_addr(int bus, int devfn, int reg)
{
    return PBM_CONFIG + ((uint64_t)bus << 16) + ((uint64_t)devfn << 8) + (uint64_t)reg;
}

static inline uint32_t pci_read32(int bus, int devfn, int reg)
{
    uint32_t v;
    __asm__ volatile("lduwa [%1] 0x1d, %0" : "=r"(v) : "r"(cfg_addr(bus, devfn, reg)) : "memory");
    return v;
}

static inline void pci_write8(int bus, int devfn, int reg, uint8_t v)
{
    __asm__ volatile("stba %0, [%1] 0x1d" : : "r"(v), "r"(cfg_addr(bus, devfn, reg)) : "memory");
}

static inline void pci_write16(int bus, int devfn, int reg, uint16_t v)
{
    __asm__ volatile("stha %0, [%1] 0x1d" : : "r"(v), "r"(cfg_addr(bus, devfn, reg)) : "memory");
}

static inline uint64_t tick(void)
{
    uint64_t v;
    __asm__ volatile("rd %%tick, %0" : "=r"(v));
    return v & ~(1ull << 63);
}

static inline void stxa_mmu(uint64_t va, int asi, uint64_t v)
{
    switch (asi) {     /* the ASI is part of the instruction */
    case ASI_IMMU: __asm__ volatile("stxa %0, [%1] 0x50; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_DMMU: __asm__ volatile("stxa %0, [%1] 0x58; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_ITLB_DATA_IN: __asm__ volatile("stxa %0, [%1] 0x54; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_DTLB_DATA_IN: __asm__ volatile("stxa %0, [%1] 0x5c; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_ITLB_DATA_ACCESS: __asm__ volatile("stxa %0, [%1] 0x55; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_DTLB_DATA_ACCESS: __asm__ volatile("stxa %0, [%1] 0x5d; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_IMMU_DEMAP: __asm__ volatile("stxa %0, [%1] 0x57; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    case ASI_DMMU_DEMAP: __asm__ volatile("stxa %0, [%1] 0x5f; membar #Sync" : : "r"(v), "r"(va) : "memory"); break;
    }
}

static inline uint64_t ldxa_tlb(int asi, int entry)
{
    uint64_t v, a = (uint64_t)entry << 3;
    switch (asi) {
    case ASI_ITLB_DATA_ACCESS: __asm__ volatile("ldxa [%1] 0x55, %0" : "=r"(v) : "r"(a)); break;
    case ASI_ITLB_TAG_READ: __asm__ volatile("ldxa [%1] 0x56, %0" : "=r"(v) : "r"(a)); break;
    case ASI_DTLB_DATA_ACCESS: __asm__ volatile("ldxa [%1] 0x5d, %0" : "=r"(v) : "r"(a)); break;
    default: __asm__ volatile("ldxa [%1] 0x5e, %0" : "=r"(v) : "r"(a)); break;
    }
    return v;
}

/* --- console: the 16550 behind the EBus ------------------------------------ */

void fw_putc(char c)
{
    for (int i = 0; i < 100000 && !(in8(UART + 5) & 0x20); i++)
        ;
    out8(UART, (uint8_t)c);
}

int fw_getc(void)
{
    return (in8(UART + 5) & 0x01) ? in8(UART) : -1;
}

static void uart_init(void)
{
    out8(UART + 1, 0);
    out8(UART + 3, 0x80);
    out8(UART + 0, 1);      /* 1.8432 MHz / 16 / 115200 */
    out8(UART + 1, 0);
    out8(UART + 3, 0x03);
    out8(UART + 4, 0x03);
}

/* --- fw_cfg ---------------------------------------------------------------- */

#define CFG_SIGNATURE     0x00
#define CFG_RAM_SIZE      0x03
#define CFG_NB_CPUS       0x05
#define CFG_KERNEL_ADDR   0x07
#define CFG_KERNEL_SIZE   0x08
#define CFG_INITRD_ADDR   0x0a
#define CFG_INITRD_SIZE   0x0b
#define CFG_BOOT_DEVICE   0x0c
#define CFG_CMDLINE_SIZE  0x14
#define CFG_CMDLINE_DATA  0x15

static void cfg_read(uint16_t key, void *buf, int n)
{
    out16le(FWCFG, key);
    for (int i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = in8(FWCFG + 1);
}

static uint64_t cfg_u64(uint16_t key, int n)
{
    uint8_t b[8] = { 0 };
    uint64_t v = 0;
    cfg_read(key, b, n);
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

/* --- device tree ----------------------------------------------------------- */

#define IH_CONSOLE 0x51530001u
#define IH_MMU     0x51530002u
#define IH_MEMORY  0x51530003u
#define PH_BIAS    0x10000u     /* phandle = node offset + PH_BIAS */

static uint8_t dt[64 * 1024] __attribute__((aligned(8)));
static char cmdline[256];
static uint64_t kernel_addr, kernel_size, initrd_addr, initrd_size;

struct range { uint64_t base, size; };
#define MAX_RANGES 16
static struct range avail[MAX_RANGES];  /* physical memory nobody claimed */
static int navail;

/* Mappings the firmware made, as the "translations" property lists them */
struct trans { uint64_t virt, size, tte; };
#define MAX_TRANS 24
static struct trans trans[MAX_TRANS];
static int ntrans;

static void prop_cells2(struct fdt_writer *w, const char *name, const uint64_t *v, int n)
{
    uint8_t b[8 * 2 * MAX_RANGES];
    for (int i = 0; i < n; i++)
        fw_put_be64(b + 8 * i, v[i]);
    fdtw_prop(w, name, b, 8 * n);
}

static void prop_u32s(struct fdt_writer *w, const char *name, const uint32_t *v, int n)
{
    uint8_t b[4 * 40];
    if (n > 40)
        n = 40;
    for (int i = 0; i < n; i++)
        fw_put_be32(b + 4 * i, v[i]);
    fdtw_prop(w, name, b, 4 * n);
}

/* A Simba (APB) PCI-PCI bridge: no ranges, as on real machines (the OS
 * reads its windows from the bridge itself) */
/* The PCI identity of a device, from its configuration space: the OS
 * builds its PCI devices from these properties */
static void pci_id_props(struct fdt_writer *w, int bus, int devfn)
{
    uint32_t id = pci_read32(bus, devfn, 0), cls = pci_read32(bus, devfn, 8);
    fdtw_prop_u32(w, "vendor-id", id & 0xffff);
    fdtw_prop_u32(w, "device-id", id >> 16);
    fdtw_prop_u32(w, "revision-id", cls & 0xff);
    fdtw_prop_u32(w, "class-code", cls >> 8);
}

static void simba_props(struct fdt_writer *w, int devfn, uint32_t bus)
{
    fdtw_prop_str(w, "name", "pci");
    fdtw_prop_str(w, "device_type", "pci");
    fdtw_prop_str(w, "model", "SUNW,simba");
    fdtw_prop_str(w, "compatible", "pci108e,5000");
    uint32_t reg[5] = { (uint32_t)devfn << 8, 0, 0, 0, 0 };
    prop_u32s(w, "reg", reg, 5);
    uint32_t range[2] = { bus, bus };
    prop_u32s(w, "bus-range", range, 2);
    pci_id_props(w, 0, devfn);
    fdtw_prop_u32(w, "#address-cells", 3);
    fdtw_prop_u32(w, "#size-cells", 2);
    fdtw_prop_u32(w, "#interrupt-cells", 1);
}

static void avail_prop(struct fdt_writer *w)
{
    uint64_t v[2 * MAX_RANGES];
    for (int i = 0; i < navail; i++) {
        v[2 * i] = avail[i].base;
        v[2 * i + 1] = avail[i].size;
    }
    prop_cells2(w, "available", v, 2 * navail);
}

/* --- PCI: the Simba bridges' bus numbers, the interrupt map registers ------- */

static int pci_ok;
static void prop_u32s(struct fdt_writer *w, const char *name, const uint32_t *v, int n);
static void pci_id_props(struct fdt_writer *w, int bus, int devfn);

static inline void pci_write32(int bus, int devfn, int reg, uint32_t v)
{
    __asm__ volatile("stwa %0, [%1] 0x1d" : : "r"(v), "r"(cfg_addr(bus, devfn, reg)) : "memory");
}

/* The devices on bus 1 (behind Simba A) and their BARs, as assigned */
struct pci_bar {
    uint8_t reg;                /* 0x10 + 4 * n */
    uint8_t io;
    uint8_t prefetch;
    uint32_t addr, size;
};
struct pci_dev {
    uint8_t devfn;
    uint32_t id, cls;
    uint8_t pin;
    int nbars;
    struct pci_bar bar[6];
};
#define MAX_PCI_DEVS 16
static struct pci_dev pdevs[MAX_PCI_DEVS];
static int npdevs;

/* Bus 1's windows: PCI I/O 0x4000-0xffff (the ISA ports and, at 0x2000,
 * the NVRAM stay below) and
 * memory from 16 MB, inside the first 2 MB / 512 MB blocks of the APB
 * address maps */
#define BUS1_IO_START   0x4000u   /* above the ISA ports and the NVRAM */
#define BUS1_IO_END     0x10000u
#define BUS1_MEM_START  0x01000000u
#define BUS1_MEM_END    0x20000000u

static void pci_assign(void)
{
    uint32_t io = BUS1_IO_START, mem = BUS1_MEM_START;
    npdevs = 0;
    for (int dev = 0; dev < 32; dev++) {
        int nfn = 1;
        for (int fn = 0; fn < nfn; fn++) {
            int devfn = dev << 3 | fn;
            uint32_t id = pci_read32(1, devfn, 0);
            if (id == 0xffffffffu || (id & 0xffff) == 0xffff || id == 0)
                continue;
            uint32_t hdr = pci_read32(1, devfn, 0x0c);
            if (fn == 0 && (hdr & 0x00800000))
                nfn = 8;
            if (devfn == EBUS_DEVFN || npdevs == MAX_PCI_DEVS)
                continue;               /* the EBus is reached through its ISA ports */
            struct pci_dev *d = &pdevs[npdevs++];
            d->devfn = (uint8_t)devfn;
            d->id = id;
            d->cls = pci_read32(1, devfn, 8);
            d->pin = (uint8_t)(pci_read32(1, devfn, 0x3c) >> 8);
            d->nbars = 0;
            for (int r = 0x10; r <= 0x24; r += 4) {
                uint32_t old = pci_read32(1, devfn, r);
                pci_write32(1, devfn, r, 0xffffffffu);
                uint32_t m = pci_read32(1, devfn, r);
                pci_write32(1, devfn, r, old);
                if (!m || m == 0xffffffffu)
                    continue;
                struct pci_bar *b = &d->bar[d->nbars];
                b->reg = (uint8_t)r;
                b->io = m & 1;
                b->prefetch = !b->io && (m & 8);
                b->size = b->io ? (~(m & ~3u) + 1) & 0xffff : ~(m & ~0xfu) + 1;
                if (!b->size)
                    continue;
                uint32_t *next = b->io ? &io : &mem;
                uint32_t a = (*next + b->size - 1) & ~(b->size - 1);
                if (a + b->size > (b->io ? BUS1_IO_END : BUS1_MEM_END))
                    continue;
                b->addr = a;
                *next = a + b->size;
                pci_write32(1, devfn, r, a);
                d->nbars++;
                if (!b->io && (m & 6) == 4)
                    r += 4;             /* a 64-bit BAR: the upper half stays 0 */
            }
            pci_write16(1, devfn, 0x04, 0x0007);    /* I/O, memory, bus master */
        }
    }
    /* Simba A forwards those windows (standard bridge registers too) */
    pci_write8(0, SIMBA_A_DEVFN, 0x1c, 0x00);               /* I/O base 0 */
    pci_write8(0, SIMBA_A_DEVFN, 0x1d, 0xf0);               /* I/O limit 0xffff */
    pci_write16(0, SIMBA_A_DEVFN, 0x20, BUS1_MEM_START >> 16);
    pci_write16(0, SIMBA_A_DEVFN, 0x22, (BUS1_MEM_END - 1) >> 16 & 0xfff0);
}

static void pci_init(void)
{
    if (pci_read32(0, SIMBA_A_DEVFN, 0) != 0x5000108eu ||
        pci_read32(0, SIMBA_B_DEVFN, 0) != 0x5000108eu) {
        fw_puts("pci: no Simba bridges behind the PBM\n");
        return;
    }
    /* Primary 0; bus 1 behind 00:01.1 (the EBus side), bus 2 behind 00:01.0 */
    pci_write8(0, SIMBA_A_DEVFN, 0x18, 0);
    pci_write8(0, SIMBA_A_DEVFN, 0x19, 1);
    pci_write8(0, SIMBA_A_DEVFN, 0x1a, 1);
    pci_write8(0, SIMBA_B_DEVFN, 0x18, 0);
    pci_write8(0, SIMBA_B_DEVFN, 0x19, 2);
    pci_write8(0, SIMBA_B_DEVFN, 0x1a, 2);
    /* APB address maps: each bit forwards one 2 MB block of PCI I/O
     * space (0xde) or one 512 MB block of memory space (0xdf). Bus 1
     * (EBus, ISA ports) gets the first of each, bus 2 the second. */
    pci_write8(0, SIMBA_A_DEVFN, 0xde, 0x01);
    pci_write8(0, SIMBA_A_DEVFN, 0xdf, 0x01);
    pci_write8(0, SIMBA_B_DEVFN, 0xde, 0x02);
    pci_write8(0, SIMBA_B_DEVFN, 0xdf, 0x02);
    pci_write16(0, SIMBA_A_DEVFN, 0x04, 0x0007);  /* I/O, memory, bus master */
    pci_write16(0, SIMBA_B_DEVFN, 0x04, 0x0007);
    uint32_t ebus = pci_read32(1, EBUS_DEVFN, 0);
    if (ebus != 0x1000108eu) {
        fw_printf("pci: no EBus at 01:01.0 (0x%08x)\n", ebus);
        return;
    }
    /* On-board interrupts: each IMAP holds the interrupt group (the
     * PBM's port id) and number that the OS reads back; not valid yet */
    for (int ino = 0x20; ino < 0x40; ino++) {
        out64(PBM_OBIO_IMAP(ino), (uint64_t)PBM_PORTID << 6 | (uint64_t)ino);
        out64(PBM_OBIO_ICLR(ino), 0);
    }
    pci_assign();
    pci_ok = 1;
}

static void idprom_read(uint8_t *b)
{
    for (int i = 0; i < 32; i++)
        b[i] = in8(NVRAM_IO + IDPROM_OFF + i);
}

/* A device on bus 1: its name and compatible from the class and IDs, reg
 * (configuration space, then each BAR's size) and assigned-addresses (the
 * BARs as programmed), and the interrupt QEMU wires it to */
static void pci_dev_node(struct fdt_writer *w, const struct pci_dev *d)
{
    char name[32], unit[16], compat[24];
    uint32_t cls = d->cls >> 8, vendor = d->id & 0xffff, device = d->id >> 16;
    const char *nm = "device", *type = 0;
    uint32_t ino = 0;
    if (cls >> 8 == 0x0200) {
        nm = "network";
        type = "network";
        ino = INO_NIC;
    } else if (cls >> 8 == 0x0101) {
        nm = "ide";
        type = "ide";
        ino = INO_IDE;
    } else if (cls >> 8 == 0x0300) {
        nm = "display";
        type = "display";
    }
    if (d->devfn & 7)
        fw_snprintf(unit, sizeof(unit), "%x,%x", d->devfn >> 3, d->devfn & 7);
    else
        fw_snprintf(unit, sizeof(unit), "%x", d->devfn >> 3);
    fw_snprintf(name, sizeof(name), "%s@%s", nm, unit);
    fw_snprintf(compat, sizeof(compat), "pci%x,%x", vendor, device);
    fdtw_begin(w, name);
    fdtw_prop_str(w, "name", nm);
    if (type)
        fdtw_prop_str(w, "device_type", type);
    fdtw_prop_str(w, "compatible", compat);
    pci_id_props(w, 1, d->devfn);
    uint32_t hi = 1u << 16 | (uint32_t)d->devfn << 8;
    uint32_t reg[5 * 7], aa[5 * 6];
    int nr = 0, na = 0;
    reg[nr++] = hi; reg[nr++] = 0; reg[nr++] = 0; reg[nr++] = 0; reg[nr++] = 0;
    for (int j = 0; j < d->nbars; j++) {
        const struct pci_bar *b = &d->bar[j];
        uint32_t sp = (b->io ? 0x01000000u : 0x02000000u) | (b->prefetch ? 0x40000000u : 0) |
                      hi | b->reg;
        reg[nr++] = sp; reg[nr++] = 0; reg[nr++] = 0; reg[nr++] = 0; reg[nr++] = b->size;
        aa[na++] = 0x80000000u | sp; aa[na++] = 0; aa[na++] = b->addr;
        aa[na++] = 0; aa[na++] = b->size;
    }
    prop_u32s(w, "reg", reg, nr);
    if (na)
        prop_u32s(w, "assigned-addresses", aa, na);
    if (ino && d->pin)
        fdtw_prop_u32(w, "interrupts", ino);
    if (ino == INO_NIC) {
        uint8_t id[32];
        idprom_read(id);
        fdtw_prop(w, "local-mac-address", id + 2, 6);
    }
    fdtw_end(w);
}

static void build_dt(void)
{
    struct fdt_writer w;
    char name[48];
    fdtw_init(&w, dt, sizeof(dt));
    fdtw_begin(&w, "");
    fdtw_prop_str(&w, "name", "SquirrelBIOS,sun4u");
    fdtw_prop_str(&w, "model", "SquirrelBIOS sun4u (QEMU)");
    fdtw_prop_str(&w, "compatible", "qemu,sun4u");
    fdtw_prop_str(&w, "banner-name", "SquirrelBIOS SPARC " SQ_VERSION);
    fdtw_prop_u32(&w, "#address-cells", 2);
    fdtw_prop_u32(&w, "#size-cells", 2);
    fdtw_prop_u32(&w, "clock-frequency", (uint32_t)tick_hz);
    uint8_t idprom[32];
    idprom_read(idprom);
    if (idprom[0] == 1)
        fdtw_prop(&w, "idprom", idprom, 32);

    fdtw_begin(&w, "openprom");
    fdtw_prop_str(&w, "name", "openprom");
    fdtw_prop_str(&w, "version", "SquirrelBIOS " SQ_VERSION);
    fdtw_prop_str(&w, "model", "SquirrelBIOS");
    fdtw_prop_empty(&w, "relative-addressing");
    fdtw_end(&w);

    fdtw_begin(&w, "chosen");
    fdtw_prop_str(&w, "name", "chosen");
    fdtw_prop_u32(&w, "stdin", IH_CONSOLE);
    fdtw_prop_u32(&w, "stdout", IH_CONSOLE);
    fdtw_prop_u32(&w, "mmu", IH_MMU);
    fdtw_prop_u32(&w, "memory", IH_MEMORY);
    fdtw_prop_str(&w, "bootargs", cmdline);
    fdtw_prop_str(&w, "bootpath", "/virtual-memory");
    fdtw_end(&w);

    fdtw_begin(&w, "options");
    fdtw_prop_str(&w, "name", "options");
    fdtw_prop_str(&w, "input-device", "ttya");
    fdtw_prop_str(&w, "output-device", "ttya");
    fdtw_prop_str(&w, "auto-boot?", "true");
    fdtw_end(&w);

    fdtw_begin(&w, "aliases");
    fdtw_prop_str(&w, "name", "aliases");
    fdtw_prop_str(&w, "ttya", SU_PATH);
    fdtw_prop_str(&w, "screen", SU_PATH);
    fdtw_end(&w);

    fdtw_begin(&w, "memory@0");
    fdtw_prop_str(&w, "name", "memory");
    fdtw_prop_str(&w, "device_type", "memory");
    uint64_t reg[2] = { 0, ram_size };
    prop_cells2(&w, "reg", reg, 2);
    avail_prop(&w);
    fdtw_end(&w);

    fdtw_begin(&w, "virtual-memory");
    fdtw_prop_str(&w, "name", "virtual-memory");
    uint64_t vavail[4] = { 0, FW_VBASE, FW_VBASE + FW_SIZE, 0xfffff80000000000ull - FW_VBASE - FW_SIZE };
    prop_cells2(&w, "available", vavail, 4);
    uint64_t t[3 * MAX_TRANS];
    for (int i = 0; i < ntrans; i++) {
        t[3 * i] = trans[i].virt;
        t[3 * i + 1] = trans[i].size;
        t[3 * i + 2] = trans[i].tte;
    }
    uint8_t tb[8 * 3 * MAX_TRANS];
    for (int i = 0; i < 3 * ntrans; i++)
        fw_put_be64(tb + 8 * i, t[i]);
    fdtw_prop(&w, "translations", tb, 24 * ntrans);
    fdtw_end(&w);

    for (int i = 0; i < ncpus; i++) {
        fw_snprintf(name, sizeof(name), "cpu@%x", i);
        fdtw_begin(&w, name);
        fdtw_prop_str(&w, "name", "SUNW,UltraSPARC-IIi");
        fdtw_prop_str(&w, "device_type", "cpu");
        fdtw_prop_u32(&w, "upa-portid", (uint32_t)i);
        fdtw_prop_u32(&w, "portid", (uint32_t)i);
        fdtw_prop_u32(&w, "reg", (uint32_t)i);
        fdtw_prop_u32(&w, "clock-frequency", (uint32_t)tick_hz);
        fdtw_prop_u32(&w, "#itlb-entries", 64);
        fdtw_prop_u32(&w, "#dtlb-entries", 64);
        fdtw_end(&w);
    }

    /* The Sabre PCI host bridge (PBM), its two Simba bridges and, on
     * the first one, the EBus with the console's 16550. Interrupts are
     * INOs of the PBM: the OS maps them with its IMAP registers. */
    fdtw_begin(&w, "pci@1fe,0");
    fdtw_prop_str(&w, "name", "pci");
    fdtw_prop_str(&w, "device_type", "pci");
    fdtw_prop_str(&w, "model", "SUNW,sabre");
    fdtw_prop_str(&w, "compatible", "pci108e,a000");
    uint64_t preg[6] = { PBM_REGS, 0x10000, PBM_CONFIG, 0x1000000, IO_BASE, 0x1000000 };
    prop_cells2(&w, "reg", preg, 6);
    fdtw_prop_u32(&w, "upa-portid", PBM_PORTID);
    fdtw_prop_u32(&w, "portid", PBM_PORTID);
    fdtw_prop_u32(&w, "#address-cells", 3);
    fdtw_prop_u32(&w, "#size-cells", 2);
    fdtw_prop_u32(&w, "#interrupt-cells", 1);
    fdtw_prop_u32(&w, "clock-frequency", 33333333);
    uint32_t cells[16];
    cells[0] = 0; cells[1] = 2;
    prop_u32s(&w, "bus-range", cells, 2);
    cells[0] = 0xc0000000u; cells[1] = 0x20000000u;
    prop_u32s(&w, "virtual-dma", cells, 2);
    cells[0] = INO_PBM_UE; cells[1] = INO_PBM_CE; cells[2] = INO_PBM_ERR;
    prop_u32s(&w, "interrupts", cells, 3);
    /* PCI I/O and memory space: child (3 cells), parent (2), size (2) */
    uint32_t ranges[14] = {
        0x01000000, 0, 0, (uint32_t)(IO_BASE >> 32), (uint32_t)IO_BASE, 0, 0x01000000,
        0x02000000, 0, 0, (uint32_t)(PBM_MEM >> 32), (uint32_t)PBM_MEM, 1, 0,
    };
    prop_u32s(&w, "ranges", ranges, 14);

    fdtw_begin(&w, "pci@1");
    simba_props(&w, SIMBA_B_DEVFN, 2);
    fdtw_end(&w);

    fdtw_begin(&w, "pci@1,1");
    simba_props(&w, SIMBA_A_DEVFN, 1);

    fdtw_begin(&w, "ebus@1");
    fdtw_prop_str(&w, "name", "ebus");
    fdtw_prop_str(&w, "compatible", "pci108e,1000");
    uint32_t ereg[5] = { 1u << 16 | EBUS_DEVFN << 8, 0, 0, 0, 0 };
    prop_u32s(&w, "reg", ereg, 5);
    pci_id_props(&w, 1, EBUS_DEVFN);
    fdtw_prop_u32(&w, "#address-cells", 2);
    fdtw_prop_u32(&w, "#size-cells", 1);
    /* Space 0x14: the ISA I/O ports, at the bottom of PCI I/O space */
    uint32_t eranges[6] = { 0x14, 0, 0x01000000, 0, 0, 0x10000 };
    prop_u32s(&w, "ranges", eranges, 6);

    fdtw_begin(&w, "su@14,3f8");
    fdtw_prop_str(&w, "name", "su");
    fdtw_prop_str(&w, "device_type", "serial");
    fdtw_prop_str(&w, "compatible", "su16550");
    uint32_t sreg[3] = { 0x14, 0x3f8, 8 };
    prop_u32s(&w, "reg", sreg, 3);
    fdtw_prop_u32(&w, "interrupts", INO_SERIAL);
    fdtw_prop_u32(&w, "clock-frequency", 1843200);
    fdtw_end(&w);               /* su */
    fdtw_end(&w);               /* ebus */
    for (int i = 0; i < npdevs; i++)
        pci_dev_node(&w, &pdevs[i]);
    fdtw_end(&w);               /* pci@1,1 */
    fdtw_end(&w);               /* pci@1fe,0 */

    fdtw_end(&w);
    if (!fdtw_finish(&w, 0))
        fw_puts("dt: device tree does not fit\n");
}

/* --- memory: claim and release --------------------------------------------- */

static void avail_remove(uint64_t base, uint64_t size)
{
    for (int i = 0; i < navail; i++) {
        uint64_t b = avail[i].base, e = b + avail[i].size, rb = base, re = base + size;
        if (re <= b || rb >= e)
            continue;
        if (rb > b && re < e) {         /* split */
            if (navail == MAX_RANGES)
                return;
            memmove(&avail[i + 2], &avail[i + 1], (navail - i - 1) * sizeof(avail[0]));
            avail[i + 1].base = re;
            avail[i + 1].size = e - re;
            avail[i].size = rb - b;
            navail++;
            i++;
            continue;
        }
        if (rb <= b && re >= e) {       /* whole range */
            memmove(&avail[i], &avail[i + 1], (navail - i - 1) * sizeof(avail[0]));
            navail--;
            i--;
            continue;
        }
        if (rb <= b) {
            avail[i].base = re;
            avail[i].size = e - re;
        } else {
            avail[i].size = rb - b;
        }
    }
}

static int avail_contains(uint64_t base, uint64_t size)
{
    for (int i = 0; i < navail; i++)
        if (base >= avail[i].base && base + size <= avail[i].base + avail[i].size)
            return 1;
    return 0;
}

static void avail_add(uint64_t base, uint64_t size)
{
    if (navail < MAX_RANGES) {
        avail[navail].base = base;
        avail[navail].size = size;
        navail++;
    }
}

/* claim: with align 0, exactly [virt, virt+size); otherwise the lowest
 * aligned free range in the 1:1-mapped RAM. Returns the address or -1. */
static uint64_t claim(uint64_t virt, uint64_t size, uint64_t align)
{
    uint64_t limit = (uint64_t)CONFIG_IDENTITY_MB << 20;
    if (!size)
        return (uint64_t)-1;
    if (!align) {
        if (!avail_contains(virt, size))
            return (uint64_t)-1;
        avail_remove(virt, size);
        return virt;
    }
    for (int i = 0; i < navail; i++) {
        uint64_t b = (avail[i].base + align - 1) & ~(align - 1);
        if (b + size <= avail[i].base + avail[i].size && b + size <= limit) {
            avail_remove(b, size);
            return b;
        }
    }
    return (uint64_t)-1;
}

/* --- TLB ------------------------------------------------------------------- */

static void tlb_lock(uint64_t va, uint64_t tte, int itlb, int dtlb)
{
    if (dtlb) {
        stxa_mmu(0x30, ASI_DMMU, va);
        stxa_mmu(0, ASI_DTLB_DATA_IN, tte);
    }
    if (itlb) {
        stxa_mmu(0x30, ASI_IMMU, va);
        stxa_mmu(0, ASI_ITLB_DATA_IN, tte);
    }
}

/* Write one entry of the 64-entry I- or D-TLB: the tag (virtual address,
 * context 0) goes through TAG_ACCESS, the data to entry 'index' */
static void tlb_load_at(uint64_t va, uint64_t tte, uint64_t index, int itlb)
{
    if (index > 63) {
        tlb_lock(va, tte, itlb, !itlb);
        return;
    }
    if (itlb) {
        stxa_mmu(0x30, ASI_IMMU, va & ~0x1fffull);
        stxa_mmu(index << 3, ASI_ITLB_DATA_ACCESS, tte);
    } else {
        stxa_mmu(0x30, ASI_DMMU, va & ~0x1fffull);
        stxa_mmu(index << 3, ASI_DTLB_DATA_ACCESS, tte);
    }
}

static void add_trans(uint64_t va, uint64_t size, uint64_t tte)
{
    if (ntrans < MAX_TRANS) {
        trans[ntrans].virt = va;
        trans[ntrans].size = size;
        trans[ntrans].tte = tte;
        ntrans++;
    }
}

static uint64_t translate(uint64_t va, int *found)
{
    for (int i = 0; i < ntrans; i++)
        if (va >= trans[i].virt && va - trans[i].virt < trans[i].size) {
            *found = 1;
            return (trans[i].tte & TTE_PA_MASK & ~(trans[i].size - 1)) + (va - trans[i].virt);
        }
    *found = 0;
    return 0;
}

/* The firmware page (locked), and RAM 1:1 below CONFIG_IDENTITY_MB in 4 MB pages */
static void map_for_os(void)
{
    uint64_t fw_tte = TTE_V | TTE_4M | TTE_L | TTE_CP | TTE_CV | TTE_P | TTE_W | TTE_G | fw_pbase;
    add_trans(FW_VBASE, FW_SIZE, fw_tte);
    uint64_t limit = (uint64_t)CONFIG_IDENTITY_MB << 20;
    if (limit > fw_pbase)
        limit = fw_pbase;
    /* Not locked: these are global, so they would cover the low user
     * addresses of every process; an OS that installs its own trap table
     * flushes the unlocked entries and maps what it wants itself */
    for (uint64_t pa = 0; pa < limit; pa += FOUR_MB) {
        uint64_t tte = TTE_V | TTE_4M | TTE_CP | TTE_CV | TTE_P | TTE_W | TTE_G | pa;
        tlb_lock(pa, tte, 1, 1);
        add_trans(pa, FOUR_MB, tte);
    }
}

/* Drop the PROM's own 1:1 instruction mapping (start.S needed it only to
 * switch the MMU on) */
static void unmap_prom(void)
{
    for (int i = 0; i < 64; i++) {
        uint64_t tag = ldxa_tlb(ASI_ITLB_TAG_READ, i);
        uint64_t data = ldxa_tlb(ASI_ITLB_DATA_ACCESS, i);
        if ((data & TTE_V) && (tag & ~0x1fffull) >= 0x1fe00000000ull)
            stxa_mmu((tag & ~0x1fffull), ASI_IMMU_DEMAP, 0);
    }
}

/* --- client interface ------------------------------------------------------- */

#define PTR(x) ((void *)(uintptr_t)(x))

static int node_ok(int ph)
{
    int off = ph - (int)PH_BIAS;
    if (ph < (int)PH_BIAS)
        return -1;
    /* must be a node start: walk the tree */
    for (int n = dtb_root(dt); n >= 0; n = dtb_next_node(dt, n, 0))
        if (n == off)
            return n;
    return -1;
}

static uint64_t ph_of(int node)
{
    return node < 0 ? 0 : (uint64_t)node + PH_BIAS;
}

static int find_device(const char *path)
{
    char buf[128];
    size_t n = 0;
    /* Strip device arguments (":a") */
    while (path[n] && path[n] != ':' && n + 1 < sizeof(buf)) {
        buf[n] = path[n];
        n++;
    }
    buf[n] = 0;
    if (buf[0] != '/') {
        /* An alias, perhaps followed by more path */
        char *rest = strchr(buf, '/');
        if (rest)
            *rest = 0;
        const char *a = dtb_prop(dt, dtb_path(dt, "/aliases"), buf, 0);
        if (!a)
            return -1;
        char full[128];
        fw_snprintf(full, sizeof(full), "%s%s%s", a, rest ? "/" : "", rest ? rest + 1 : "");
        return dtb_path(dt, full);
    }
    return dtb_path(dt, buf);
}

static int node_path(int node, char *buf, int len)
{
    char tmp[256];
    int pos = (int)sizeof(tmp) - 1;
    tmp[pos] = 0;
    if (node == dtb_root(dt))
        tmp[--pos] = '/';
    for (int n = node; n >= 0 && n != dtb_root(dt); n = dtb_parent(dt, n)) {
        const char *nm = dtb_name(dt, n);
        int l = (int)strlen(nm);
        if (pos - l - 1 < 0)
            return -1;
        pos -= l;
        memcpy(tmp + pos, nm, l);
        tmp[--pos] = '/';
    }
    int total = (int)strlen(tmp + pos);
    if (buf && len > 0) {
        int c = total + 1 < len ? total + 1 : len;
        memcpy(buf, tmp + pos, c);
        buf[c - 1] = 0;
    }
    return total;
}

static int ih_package(uint64_t ih)
{
    switch (ih) {
    case IH_CONSOLE: return dtb_path(dt, SU_PATH);
    case IH_MMU: return dtb_path(dt, "/virtual-memory");
    case IH_MEMORY: return dtb_path(dt, "/memory");
    }
    return -1;
}

static int64_t ms_now(void)
{
    return (int64_t)(tick() / (tick_hz / 1000));
}

static int exit_requested;
static void prompt_after_exit(void) __attribute__((noreturn));

/* call-method on the mmu and memory packages. a = arguments after the
 * method name and ihandle (stack order: the last argument first), r =
 * returns after catch-result. Returns the number of results. */
static int call_method(const char *m, uint64_t ih, uint64_t *a, int na, uint64_t *r)
{
    if (ih == IH_MMU && !strcmp(m, "translate") && na >= 1) {
        int found;
        uint64_t pa = translate(a[0], &found);
        if (!found) {
            r[0] = 0;               /* false */
            return 1;
        }
        /* ( virt -- phys.lo phys.hi mode true ), top of stack first */
        r[0] = (uint64_t)-1;
        r[1] = 0;
        r[2] = pa >> 32;
        r[3] = pa & 0xffffffffull;
        return 4;
    }
    if (ih == IH_MMU && (!strcmp(m, "SUNW,dtlb-load") || !strcmp(m, "SUNW,itlb-load")) && na >= 3) {
        /* Arguments as Linux passes them: virt, data, index (the TLB
         * entry to write) */
        tlb_load_at(a[0], a[1], a[2], m[5] == 'i');
        return 0;
    }
    if (ih == IH_MEMORY && !strcmp(m, "claim") && na >= 2) {
        /* ( [phys.lo phys.hi] size align -- base.hi base.lo ), last
         * argument first: align, size, phys.hi, phys.lo */
        uint64_t align = a[0], size = a[1];
        uint64_t phys = na >= 4 ? (a[2] << 32 | (a[3] & 0xffffffffull)) : 0;
        uint64_t base = claim(phys, size, align);
        if (base == (uint64_t)-1)
            return -1;
        r[0] = base >> 32;
        r[1] = base & 0xffffffffull;
        return 2;
    }
    if (ih == IH_MEMORY && !strcmp(m, "release") && na >= 2) {
        /* ( phys.lo phys.hi size -- ): size, phys.hi, phys.lo */
        avail_add(na >= 3 ? (a[1] << 32 | (a[2] & 0xffffffffull)) : a[1], a[0]);
        return 0;
    }
    return -1;
}

int64_t cif_handler(uint64_t *args)
{
    const char *svc = PTR(args[0]);
    int nargs = (int)args[1], nret = (int)args[2];
    uint64_t *a = &args[3], *r = &args[3 + nargs];
    int node;
    (void)nret;

#define IS(s) (!strcmp(svc, s))
    if (IS("test")) {
        static const char *const known[] = {
            "test", "peer", "child", "parent", "getproplen", "getprop", "nextprop",
            "finddevice", "instance-to-package", "package-to-path", "instance-to-path",
            "canon", "open", "close", "read", "write", "seek", "claim", "release",
            "milliseconds", "call-method", "exit", "enter", "boot", "interpret",
            "SUNW,set-trap-table", "SUNW,power-off", 0
        };
        const char *q = PTR(a[0]);
        r[0] = 1;
        for (int i = 0; known[i]; i++)
            if (!strcmp(q, known[i]))
                r[0] = 0;
    } else if (IS("peer")) {
        if (a[0] == 0)
            r[0] = ph_of(dtb_root(dt));
        else
            r[0] = (node = node_ok((int)a[0])) < 0 ? 0 : ph_of(dtb_next_sibling(dt, node));
    } else if (IS("child")) {
        r[0] = (node = node_ok((int)a[0])) < 0 ? 0 : ph_of(dtb_first_child(dt, node));
    } else if (IS("parent")) {
        r[0] = (node = node_ok((int)a[0])) < 0 ? 0 : ph_of(dtb_parent(dt, node));
    } else if (IS("finddevice")) {
        node = find_device(PTR(a[0]));
        r[0] = node < 0 ? (uint64_t)-1 : ph_of(node);
    } else if (IS("getproplen") || IS("getprop")) {
        int len;
        node = node_ok((int)a[0]);
        const void *p = node < 0 ? 0 : dtb_prop(dt, node, PTR(a[1]), &len);
        if (!p) {
            r[0] = (uint64_t)-1;
        } else {
            if (IS("getprop")) {
                uint64_t n = (uint64_t)len < a[3] ? (uint64_t)len : a[3];
                memcpy(PTR(a[2]), p, n);
            }
            r[0] = (uint64_t)len;
        }
    } else if (IS("nextprop")) {
        const char *prev = PTR(a[1]), *name;
        char *buf = PTR(a[2]);
        node = node_ok((int)a[0]);
        if (node < 0) {
            r[0] = (uint64_t)-1;
        } else {
            int p = dtb_next_prop(dt, node, -1, &name, 0, 0);
            if (prev && *prev) {
                while (p >= 0 && strcmp(name, prev))
                    p = dtb_next_prop(dt, node, p, &name, 0, 0);
                if (p >= 0)
                    p = dtb_next_prop(dt, node, p, &name, 0, 0);
            }
            if (p < 0) {
                buf[0] = 0;
                r[0] = 0;
            } else {
                memcpy(buf, name, strlen(name) + 1);
                r[0] = 1;
            }
        }
    } else if (IS("instance-to-package")) {
        node = ih_package(a[0]);
        r[0] = node < 0 ? (uint64_t)-1 : ph_of(node);
    } else if (IS("package-to-path") || IS("instance-to-path")) {
        node = IS("package-to-path") ? node_ok((int)a[0]) : ih_package(a[0]);
        r[0] = node < 0 ? (uint64_t)-1 : (uint64_t)node_path(node, PTR(a[1]), (int)a[2]);
    } else if (IS("canon")) {
        node = find_device(PTR(a[0]));
        r[0] = node < 0 ? (uint64_t)-1 : (uint64_t)node_path(node, PTR(a[1]), (int)a[2]);
    } else if (IS("open")) {
        node = find_device(PTR(a[0]));
        r[0] = node == ih_package(IH_CONSOLE) ? IH_CONSOLE :
               node == ih_package(IH_MMU) ? IH_MMU :
               node == ih_package(IH_MEMORY) ? IH_MEMORY : 0;
    } else if (IS("close")) {
        /* nothing to do */
    } else if (IS("write")) {
        const char *buf = PTR(a[1]);
        if (a[0] != IH_CONSOLE) {
            r[0] = (uint64_t)-1;
        } else {
            for (uint64_t i = 0; i < a[2]; i++)
                fw_putc(buf[i]);
            r[0] = a[2];
        }
    } else if (IS("read")) {
        char *buf = PTR(a[1]);
        uint64_t n = 0;
        if (a[0] != IH_CONSOLE) {
            r[0] = (uint64_t)-1;
        } else {
            for (int c; n < a[2] && (c = fw_getc()) >= 0; n++)
                buf[n] = (char)c;
            r[0] = n ? n : (uint64_t)-2;    /* -2: nothing yet */
        }
    } else if (IS("seek")) {
        r[0] = (uint64_t)-1;
    } else if (IS("claim")) {
        /* ( virt size align -- base ): 1:1, so virt and phys agree */
        r[0] = claim(a[0], a[1], a[2]);
    } else if (IS("release")) {
        avail_add(a[0], a[1]);
    } else if (IS("milliseconds")) {
        r[0] = (uint64_t)ms_now();
    } else if (IS("call-method")) {
        uint64_t res[4] = { 0 };
        /* args: method, ihandle, then the method's arguments */
        int n = call_method(PTR(a[0]), a[1], &a[2], nargs - 2, res);
        if (n < 0) {
            r[0] = (uint64_t)-1;    /* catch-result: the method failed */
        } else {
            r[0] = 0;
            for (int i = 0; i < n && i + 1 < nret; i++)
                r[1 + i] = res[i];
        }
    } else if (IS("interpret")) {
        r[0] = (uint64_t)-1;        /* no Forth here */
    } else if (IS("SUNW,set-trap-table")) {
        fw_set_tba(a[0]);
    } else if (IS("exit") || IS("enter") || IS("boot")) {
        fw_printf("\ncif: %s\n", svc);
        exit_requested = 1;
        prompt_after_exit();
    } else if (IS("SUNW,power-off")) {
        fw_puts("\ncif: power off (not wired up on QEMU sun4u), halting\n");
        for (;;)
            ;
    } else {
        return -1;
    }
    return 0;
#undef IS
}

/* --- traps before the OS ---------------------------------------------------- */

void fw_trap(const uint64_t *s)
{
    fw_printf("trap: type 0x%lx at tpc 0x%lx tnpc 0x%lx tstate 0x%lx tl %lu, halting\n",
              s[0], s[1], s[2], s[3], s[4]);
    for (;;)
        ;
}

/* --- boot -------------------------------------------------------------------- */

static uint64_t entry_point;

/* Linux on sparc64 finds its initrd in the "HdrS" header near its start,
 * which a boot loader fills in: the 32-bit image field (offset 16) and
 * size (20) and, from header version 0x0300, a 64-bit image field (40).
 * The address is given relative to the lowest available physical page,
 * plus the kernel's base 0x400000, the way the header is defined. */
#define HDRS_KERNBASE 0x400000ull

static int set_hdrs_initrd(void)
{
    uint8_t *k = PTR(kernel_addr);
    for (uint64_t off = 0; off + 48 <= 512 && off + 48 <= kernel_size; off += 4) {
        if (memcmp(k + off, "HdrS", 4))
            continue;
        uint8_t *h = k + off;
        uint16_t ver = (uint16_t)(h[8] << 8 | h[9]);
        uint64_t low = ~0ull;
        for (int i = 0; i < navail; i++)
            if (avail[i].base < low)
                low = avail[i].base;
        uint64_t img = initrd_addr - low + HDRS_KERNBASE;
        fw_put_be32(h + 20, (uint32_t)initrd_size);
        if (ver >= 0x0300) {
            fw_put_be32(h + 16, 0);
            fw_put_be64(h + 40, img);
        } else {
            fw_put_be32(h + 16, (uint32_t)img);
        }
        return 1;
    }
    return 0;
}

static void boot_kernel(void)
{
    if (!kernel_addr || !kernel_size) {
        fw_puts("boot: no kernel (-kernel)\n");
        return;
    }
    uint64_t limit = (uint64_t)CONFIG_IDENTITY_MB << 20;
    if (kernel_addr < 0x4000 || kernel_addr + kernel_size > limit) {
        fw_printf("boot: kernel at 0x%lx-0x%lx is outside the mapped RAM below %u MB\n",
                  kernel_addr, kernel_addr + kernel_size, CONFIG_IDENTITY_MB);
        return;
    }
    entry_point = kernel_addr;
    /* The kernel's 4 MB-aligned span is in use */
    uint64_t kb = kernel_addr & ~(FOUR_MB - 1);
    avail_remove(kb, ((kernel_addr + kernel_size + FOUR_MB - 1) & ~(FOUR_MB - 1)) - kb);
    if (initrd_size) {
        avail_remove(initrd_addr, initrd_size);
        fw_printf("boot: initrd at 0x%lx, %lu bytes%s\n", initrd_addr, initrd_size,
                  set_hdrs_initrd() ? ", in the kernel's HdrS header" : "");
    }
    build_dt();
    unmap_prom();
    fw_printf("boot: kernel at 0x%lx, %lu bytes, client interface at 0x%lx\n", kernel_addr,
              kernel_size, (uint64_t)(uintptr_t)cif_entry);
    fw_puts("jumping to kernel\n");
    fw_enter(entry_point, (uint64_t)(uintptr_t)cif_entry, initrd_addr, initrd_size);
}

/* --- prompt ------------------------------------------------------------------ */

static void cmd_info(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_printf("SquirrelBIOS %s (sparc64-sun4u)\n", SQ_VERSION);
    fw_printf("ram: %lu MB, firmware at 0x%lx (physical 0x%lx)\n", ram_size >> 20, FW_VBASE,
              fw_pbase);
    fw_printf("cpus: %d, tick %lu Hz\n", ncpus, tick_hz);
    for (int i = 0; i < navail; i++)
        fw_printf("available: 0x%lx-0x%lx\n", avail[i].base, avail[i].base + avail[i].size - 1);
}

static void cmd_dt(int argc, char **argv)
{
    int node = dtb_path(dt, argc > 1 ? argv[1] : "/");
    if (node < 0)
        fw_printf("dt: no node %s\n", argv[1]);
    else
        dtb_print(dt, node);
}

static void cmd_pci(int argc, char **argv)
{
    (void)argc, (void)argv;
    for (int i = 0; i < npdevs; i++) {
        const struct pci_dev *d = &pdevs[i];
        fw_printf("01:%02x.%x %04x:%04x class %06x pin %u\n", d->devfn >> 3, d->devfn & 7,
                  d->id & 0xffff, d->id >> 16, d->cls >> 8, d->pin);
        for (int j = 0; j < d->nbars; j++)
            fw_printf("  bar 0x%x: %s 0x%x, %u bytes%s\n", d->bar[j].reg,
                      d->bar[j].io ? "i/o" : "mem", d->bar[j].addr, d->bar[j].size,
                      d->bar[j].prefetch ? ", prefetchable" : "");
    }
}

static void cmd_boot(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (exit_requested)
        fw_puts("boot: the kernel has already run; reset to boot it again\n");
    else
        boot_kernel();
}

static const struct fw_cmd commands[] = {
    { "info", "firmware, memory and CPUs", cmd_info },
    { "dt", "print the device tree (or one node: dt /chosen)", cmd_dt },
    { "pci", "the PCI devices on bus 1 and their BARs", cmd_pci },
    { "boot", "boot the -kernel image", cmd_boot },
    { 0, 0, 0 },
};

static void prompt_after_exit(void)
{
    /* Back from an OS: our own trap table again */
    fw_set_tba((uint64_t)(uintptr_t)trap_table);
    fw_puts("type help for commands\n");
    fw_prompt(commands);
    for (;;)
        ;
}

/* --- main -------------------------------------------------------------------- */

void fw_main(uint64_t pbase, uint64_t ram)
{
    fw_pbase = pbase;
    ram_size = ram;
    uart_init();
    fw_printf("\nSquirrelBIOS SPARC %s (sparc64-sun4u)\n", SQ_VERSION);

    char sig[4];
    cfg_read(CFG_SIGNATURE, sig, 4);
    if (memcmp(sig, "QEMU", 4))
        fw_puts("fw_cfg: not found\n");
    ncpus = (int)cfg_u64(CFG_NB_CPUS, 2);
    if (ncpus < 1)
        ncpus = 1;
    kernel_addr = cfg_u64(CFG_KERNEL_ADDR, 8);
    kernel_size = cfg_u64(CFG_KERNEL_SIZE, 8);
    initrd_addr = cfg_u64(CFG_INITRD_ADDR, 8);
    initrd_size = cfg_u64(CFG_INITRD_SIZE, 8);
    uint32_t clen = (uint32_t)cfg_u64(CFG_CMDLINE_SIZE, 4);
    if (clen >= sizeof(cmdline))
        clen = sizeof(cmdline) - 1;
    cfg_read(CFG_CMDLINE_DATA, cmdline, (int)clen);
    cmdline[clen] = 0;

    /* %tick rate: QEMU's sun4u CPUs count at their clock frequency */
    fw_printf("mem: %lu MB, firmware at 0x%lx (physical 0x%lx)\n", ram >> 20, FW_VBASE, pbase);
    fw_printf("cpu: %d, mmu on, trap table at 0x%lx\n", ncpus, (uint64_t)(uintptr_t)trap_table);

    /* Free RAM: all of it but page 0, the firmware's 4 MB */
    navail = 0;
    avail_add(0x4000, pbase - 0x4000);
    if (pbase + FW_SIZE < ram)
        avail_add(pbase + FW_SIZE, ram - pbase - FW_SIZE);
    map_for_os();
    pci_init();
    fw_printf("pci: sabre at 0x%lx, ebus %s, console interrupt 0x%x (imap 0x%lx)\n", (uint64_t)PBM_REGS,
              pci_ok ? "on bus 1" : "missing", INO_SERIAL, in64(PBM_OBIO_IMAP(INO_SERIAL)));
    build_dt();

    if (cmdline[0])
        fw_printf("bootargs: %s\n", cmdline);
    boot_kernel();
    fw_puts("type help for commands\n");
    fw_prompt(commands);
}
