/* SquirrelBIOS UEFI core (common/efi): PCI Express host bridge.
 * Original code, from the PCI Express Base specification (enhanced
 * configuration access, type 0 headers, BARs) and the devicetree PCI bus
 * binding (pci-host-ecam-generic: reg, ranges).
 *
 * The device tree's ECAM bridge has nothing behind it set up: the firmware
 * walks bus 0 (QEMU's virt boards put every device there), sizes each
 * function's memory BARs, places them in the bridge's 32-bit window (the
 * 64-bit one when that fills), turns on memory decoding and bus mastering
 * and hands the function to the drivers (pci_disk_add). I/O BARs
 * and bridges behind bus 0 are left alone. */
#include "efi.h"

#define CFG_ID       0x00
#define CFG_COMMAND  0x04
#define CFG_CLASS    0x08
#define CFG_HEADER   0x0E
#define CFG_BAR0     0x10

#define CMD_MEMORY   0x0002
#define CMD_MASTER   0x0004

#define PNP0A03      0x0A0341D0     /* EISA ID of a PCI root bridge */

struct window {
    u64 pci, cpu, size, used;
};

static struct window mem32, mem64;

/* An aligned piece of a window: its PCI address in *pci, the CPU's */
static int take(struct window *w, u64 size, u64 *pci)
{
    if (!w->size)
        return 0;
    u64 at = (w->pci + w->used + size - 1) & ~(size - 1);
    if (at + size > w->pci + w->size)
        return 0;
    w->used = at + size - w->pci;
    *pci = at;
    return 1;
}

/* The bridge's memory windows, from "ranges": 3 PCI address cells, 2 CPU
 * address cells (QEMU's virt boards), 2 size cells */
static void windows(int node)
{
    u32 len;
    const u8 *r = fdt_prop(node, "ranges", &len);
    for (u32 off = 0; r && off + 28 <= len; off += 28) {
        const u8 *e = r + off;
        u32 space = (be32(e) >> 24) & 3;
        struct window w = { be64(e + 4), be64(e + 12), be64(e + 20), 0 };
        if (space == 2 && !mem32.size)
            mem32 = w;
        else if (space == 3 && !mem64.size)
            mem64 = w;
    }
}

static u64 to_cpu(u64 pci)
{
    struct window *w = pci >= mem32.pci && pci - mem32.pci < mem32.size ? &mem32 : &mem64;
    return pci - w->pci + w->cpu;
}

static u32 ecam_read32(struct sq_pci *p, u32 off)
{
    return mmio_read32(p->priv + off);
}

/* Size and place one BAR; the next BAR's index */
static int bar(struct sq_pci *p, int i)
{
    uptr a = p->priv + CFG_BAR0 + 4 * i;
    u32 orig = mmio_read32(a);
    if (orig & 1)
        return i + 1;                       /* I/O space */
    int wide = ((orig >> 1) & 3) == 2;
    mmio_write32(a, 0xFFFFFFFF);
    u64 mask = mmio_read32(a) & ~0xFull;
    if (wide && i < 5) {
        mmio_write32(a + 4, 0xFFFFFFFF);
        mask |= (u64)mmio_read32(a + 4) << 32;
    } else {
        mask |= 0xFFFFFFFF00000000ull;
        wide = 0;
    }
    u64 size = ~mask + 1;
    u64 at = 0;
    if (!mask || !(size && (size & (size - 1)) == 0)
        || !(take(&mem32, size, &at) || (wide && take(&mem64, size, &at)))) {
        mmio_write32(a, orig);
        return i + 1 + wide;
    }
    mmio_write32(a, (u32)at | (orig & 0xF));
    if (wide)
        mmio_write32(a + 4, (u32)(at >> 32));
    p->bar[i] = to_cpu(at);
    return i + 1 + wide;
}

/* ACPI(PNP0A03,0)/Pci(dev,fn) */
static EFI_DEVICE_PATH_PROTOCOL *pci_path(u8 dev, u8 fn)
{
    static const u8 root[16] = {
        ACPI_DEVICE_PATH, ACPI_DP, 12, 0, 0xD0, 0x41, 0x03, 0x0A, 0, 0, 0, 0,
        END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0
    };
    _Static_assert(PNP0A03 == 0x0A0341D0, "PNP0A03 node bytes");
    EFI_DEVICE_PATH_PROTOCOL *node = efi_dp_pci(dev, fn);
    return node ? efi_dp_append((const EFI_DEVICE_PATH_PROTOCOL *)root, node) : NULL;
}

static int function(uptr ecam, u8 dev, u8 fn)
{
    struct sq_pci p;
    memset(&p, 0, sizeof p);
    uptr cfg = ecam + ((uptr)dev << 15 | (uptr)fn << 12);
    p.priv = cfg;
    p.read32 = ecam_read32;
    u32 id = mmio_read32(cfg + CFG_ID);
    if ((id & 0xFFFF) == 0xFFFF)
        return 0;
    if ((mmio_read8(cfg + CFG_HEADER) & 0x7F) != 0)
        return 0;                           /* a bridge */
    p.dev = dev;
    p.fn = fn;
    p.vendor = (u16)id;
    p.device = (u16)(id >> 16);
    p.class_rev = mmio_read32(cfg + CFG_CLASS);
    if (p.class_rev >> 24 != 0x01)
        return 0;                           /* not mass storage */

    u16 cmd = mmio_read16(cfg + CFG_COMMAND);
    mmio_write16(cfg + CFG_COMMAND, cmd & ~(CMD_MEMORY | 1));
    for (int i = 0; i < 6;)
        i = bar(&p, i);
    mmio_write16(cfg + CFG_COMMAND, cmd | CMD_MEMORY | CMD_MASTER);

    p.path = pci_path(dev, fn);
    return p.path ? pci_disk_add(&p) : 0;
}

/* Every disk on the device tree's ECAM host bridge */
int pci_probe(void)
{
    int node = fdt_find_compatible("pci-host-ecam-generic", -1);
    u64 ecam, size;
    if (node < 0 || !fdt_reg(node, 0, &ecam, &size))
        return 0;
    windows(node);
    if (!mem32.size && !mem64.size)
        return 0;
    int n = 0;
    for (u8 dev = 0; dev < 32; dev++) {
        u8 head = mmio_read8((uptr)ecam + ((uptr)dev << 15) + CFG_HEADER);
        int fns = head == 0xFF ? 0 : (head & 0x80) ? 8 : 1;
        for (u8 fn = 0; fn < fns; fn++)
            n += function((uptr)ecam, dev, fn);
    }
    return n;
}
