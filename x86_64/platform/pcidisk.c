/* SquirrelBIOS x86_64-uefi: PCI mass storage for the shared disk drivers
 * (common/efi: nvme.c, ahci.c, virtio_pci.c). Original code. Walks every
 * bus through configuration mechanism 1 (pci.c), places the memory BARs
 * of each mass storage function below 4 GB, turns on memory decoding and
 * bus mastering, and offers the function to pci_disk_add. A transitional
 * virtio-blk with its legacy I/O BAR is virtio_blk.c's. */
#include "efi.h"

#define CFG_COMMAND  0x04
#define CFG_CLASS    0x08
#define CFG_HEADER   0x0E
#define CFG_BAR0     0x10

static u32 cfg_read32(struct sq_pci *p, u32 off)
{
    return pci_read32(p->bus, p->dev, p->fn, (u8)off);
}

static int function(u8 bus, u8 dev, u8 fn)
{
    u32 id = pci_read32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || (pci_read8(bus, dev, fn, CFG_HEADER) & 0x7F) != 0)
        return 0;
    u32 class_rev = pci_read32(bus, dev, fn, CFG_CLASS);
    if (class_rev >> 24 != 0x01 && class_rev >> 8 != 0x0C0330 && class_rev >> 16 != 0x0805)
        return 0;                           /* not mass storage, xHCI or an SD host */
    if ((id & 0xFFFF) == 0x1AF4 && (id >> 16) == 0x1001 && (pci_read32(bus, dev, fn, CFG_BAR0) & 1))
        return 0;                           /* legacy virtio-blk */

    struct sq_pci p;
    memset(&p, 0, sizeof p);
    p.bus = bus;
    p.dev = dev;
    p.fn = fn;
    p.vendor = (u16)id;
    p.device = (u16)(id >> 16);
    p.class_rev = class_rev;
    p.read32 = cfg_read32;
    for (int i = 0; i < 6; i++) {
        u8 off = (u8)(CFG_BAR0 + 4 * i);
        u32 bar = pci_read32(bus, dev, fn, off);
        if (bar & 1)
            continue;                       /* I/O */
        p.bar[i] = pci_assign_mem_bar(bus, dev, fn, off);
        if ((bar & 6) == 4)
            i++;                            /* 64-bit: its upper half */
    }
    pci_write16(bus, dev, fn, CFG_COMMAND, pci_read16(bus, dev, fn, CFG_COMMAND) | 0x06);
    p.path = efi_dp_pci(dev, fn);
    return p.path ? pci_disk_add(&p) : 0;
}

int pci_disk_probe(void)
{
    int n = 0;
    for (u32 bus = 0; bus < 256; bus++)
        for (u8 dev = 0; dev < 32; dev++) {
            if ((pci_read32((u8)bus, dev, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            int fns = (pci_read8((u8)bus, dev, 0, CFG_HEADER) & 0x80) ? 8 : 1;
            for (u8 fn = 0; fn < fns; fn++)
                n += function((u8)bus, dev, fn);
        }
    return n;
}
