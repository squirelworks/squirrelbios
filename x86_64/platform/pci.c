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

void pci_write8(u8 bus, u8 dev, u8 fn, u8 off, u8 v)
{
    u32 cur = pci_read32(bus, dev, fn, off);
    u32 shift = (off & 3) * 8;
    cur = (cur & ~(0xFFu << shift)) | ((u32)v << shift);
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

/* 32-bit memory BARs: bump downward from 0xE0000000 so they sit below
 * the PCI hole. Prefetchable bit is preserved. Returns 0 on failure. */
static u32 mem_bump = 0xE0000000;

u32 pci_assign_mem_bar(u8 bus, u8 dev, u8 fn, u8 bar_off)
{
    u32 bar = pci_read32(bus, dev, fn, bar_off);
    if (bar & 1)
        return 0;                               /* I/O BAR */
    if ((bar & 0x6) == 0x4)
        return 0;                               /* 64-bit: skip for now */
    u32 cur = bar & 0xFFFFFFF0;
    if (cur && cur != 0xFFFFFFF0)
        return cur;
    pci_write32(bus, dev, fn, bar_off, 0xFFFFFFFF);
    u32 mask = pci_read32(bus, dev, fn, bar_off);
    u32 size = ~(mask & 0xFFFFFFF0) + 1;
    if (!size || size > 0x10000000)
        return 0;
    u32 base = (mem_bump - size) & ~(size - 1);
    mem_bump = base;
    pci_write32(bus, dev, fn, bar_off, base | (bar & 0xF));
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

/* Extra PCI root buses: QEMU's pxb and pxb-pcie expanders (roadmap F1).
 * fw_cfg etc/extra-pci-roots counts them. Their bus numbers are not in
 * the expanders' config space, so they are found as the populated buses
 * past 0 that no bridge leads to, on bus 0 or on a root found before
 * (this firmware numbers no bridges, but an OS or option ROM before a
 * warm reset may have). Ascending bus numbers. */
/* Mark the buses that bridges on bus b lead to (as numbered so far) */
static void mark_behind(u8 b, u8 *behind)
{
    for (u8 d = 0; d < 32; d++) {
        if ((pci_read32(b, d, 0, 0) & 0xFFFF) == 0xFFFF || (pci_read8(b, d, 0, 0x0E) & 0x7F) != 1)
            continue;
        u8 sec = pci_read8(b, d, 0, 0x19), sub = pci_read8(b, d, 0, 0x1A);
        for (u32 x = sec; sec > b && x <= sub; x++)
            behind[x] = 1;
    }
}

int pci_extra_roots(u8 *roots, int max)
{
    u16 sel;
    u32 size;
    u64 want = 0;
    if (!fwcfg_find("etc/extra-pci-roots", &sel, &size) || size != 8)
        return 0;
    fwcfg_read(sel, &want, 8);
    u8 behind[256] = { 0 };
    mark_behind(0, behind);
    int n = 0;
    for (u32 b = 1; b < 256 && (u64)n < want && n < max; b++) {
        if (behind[b])
            continue;
        for (u8 d = 0; d < 32; d++)
            if ((pci_read32((u8)b, d, 0, 0) & 0xFFFF) != 0xFFFF) {
                roots[n++] = (u8)b;
                mark_behind((u8)b, behind);     /* a root's own bridges */
                break;
            }
    }
    return n;
}

