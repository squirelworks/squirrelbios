/* SquirrelBIOS x86_64-uefi: PCI configuration space over 0xCF8/0xCFC.
 * Original code. */
#include "efi.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    outl(PCI_ADDR, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
         ((u32)fn << 8) | (off & 0xFC));
    return inl(PCI_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off)
{
    return (u16)(pci_read32(bus, dev, fn, off) >> ((off & 2) * 8));
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off)
{
    return (u8)(pci_read32(bus, dev, fn, off) >> ((off & 3) * 8));
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    outl(PCI_ADDR, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
         ((u32)fn << 8) | (off & 0xFC));
    outl(PCI_DATA, v);
}

void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 v)
{
    u32 cur = pci_read32(bus, dev, fn, off);
    u32 shift = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((u32)v << shift);
    pci_write32(bus, dev, fn, off, cur);
}

/* Assign an I/O BAR that QEMU left floating (all ones). Returns the base
 * or 0 on failure. Uses a bump allocator from 0xC000 upward. */
static u16 io_bump = 0xC000;

u16 pci_assign_io_bar(u8 bus, u8 dev, u8 fn, u8 bar_off)
{
    u32 bar = pci_read32(bus, dev, fn, bar_off);
    if (!(bar & 1))
        return 0;                               /* memory BAR */
    u16 cur = (u16)(bar & 0xFFFC);
    if (cur && cur != 0xFFFC)
        return cur;                             /* already placed */
    pci_write32(bus, dev, fn, bar_off, 0xFFFFFFFF);
    u32 mask = pci_read32(bus, dev, fn, bar_off);
    u16 size = (u16)((~(mask & 0xFFFC) + 1) & 0xFFFC);
    if (!size)
        size = 0x80;
    u16 base = (io_bump + size - 1) & ~(size - 1);
    if (base < 0xC000)
        base = 0xC000;
    io_bump = base + size;
    pci_write32(bus, dev, fn, bar_off, base | 1);
    return base;
}

int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn)
{
    for (u16 b = 0; b < 256; b++) {
        for (u8 d = 0; d < 32; d++) {
            u8 maxfn = 1;
            u32 id0 = pci_read32((u8)b, d, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;
            if (pci_read8((u8)b, d, 0, 0x0E) & 0x80)
                maxfn = 8;
            for (u8 f = 0; f < maxfn; f++) {
                u32 id = pci_read32((u8)b, d, f, 0);
                if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                    *bus = (u8)b;
                    *dev = d;
                    *fn = f;
                    return 1;
                }
            }
        }
    }
    return 0;
}
