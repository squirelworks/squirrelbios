/* SquirrelBIOS x86_64-uefi: virtio-blk (legacy PCI interface).
 * Original code, from the VIRTIO 1.1 legacy description (I/O BAR0).
 * One request at a time on descriptors 0-2. */
#include "efi.h"

#define VBLK_VENDOR 0x1AF4
#define VBLK_DEVICE 0x1001

#define VIO_HOST_F   0x00
#define VIO_GUEST_F  0x04
#define VIO_QPFN     0x08
#define VIO_QSIZE    0x0C
#define VIO_QSEL     0x0E
#define VIO_QNOTIFY  0x10
#define VIO_STATUS   0x12
#define VIO_CONFIG   0x14

#define VIO_ACK      0x01
#define VIO_DRIVER   0x02
#define VIO_DRIVER_OK 0x04

#define VRING_NEXT   1
#define VRING_WRITE  2
#define VBLK_T_IN    0
#define VBLK_T_OUT   1

struct vblk {
    struct sq_blk blk;          /* first: the disk disk.c publishes */
    u16 io;
    u8 bus, dev, fn;
    u16 qsize;
    u16 last_used;
    u64 sectors;
    u8 *queue;                  /* three pages: desc + avail + used */
    u8 *hdr;
    u8 *status_byte;
    u16 avail_off;
    u16 used_off;
};

static struct vblk disk;

static void w32(u16 port, u32 v) { outl(port, v); }
static u32 r32(u16 port) { return inl(port); }
static void w16(u16 port, u16 v) { outw(port, v); }
static u16 r16(u16 port) { return inw(port); }
static void w8(u16 port, u8 v) { outb(port, v); }

static void desc_set(u8 *d, u64 addr, u32 len, u16 flags, u16 next)
{
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}

static int vblk_init(struct vblk *v, u16 io, u8 bus, u8 dev, u8 fn)
{
    v->io = io;
    v->bus = bus;
    v->dev = dev;
    v->fn = fn;
    w8(io + VIO_STATUS, 0);
    w8(io + VIO_STATUS, VIO_ACK);
    w8(io + VIO_STATUS, VIO_ACK | VIO_DRIVER);
    w32(io + VIO_GUEST_F, 0);
    w16(io + VIO_QSEL, 0);
    u16 qsz = r16(io + VIO_QSIZE);
    if (qsz < 3 || qsz > 256 || (qsz & (qsz - 1)))
        return 0;
    v->qsize = qsz;

    /* Descriptors + available + used rings need up to three pages for a
     * 256-entry queue; keep the request header and status in a fourth. */
    EFI_PHYSICAL_ADDRESS pages = 0;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, 4, &pages) != EFI_SUCCESS)
        return 0;
    v->queue = (u8 *)(uptr)pages;
    memset(v->queue, 0, 4 * 4096);

    u32 avail = (u32)qsz * 16;
    v->avail_off = (u16)avail;
    u32 used = (avail + 6 + 2 * (u32)qsz + 0xFFF) & ~0xFFFu;
    if (used + 6 + 8 * (u32)qsz > 3 * 4096) {
        efi_free_pages(pages, 4);
        return 0;
    }
    v->used_off = (u16)used;

    v->hdr = v->queue + 3 * 4096;
    v->status_byte = v->hdr + 16;
    v->last_used = 0;

    desc_set(v->queue + 0, (u64)(uptr)v->hdr, 16, VRING_NEXT, 1);
    desc_set(v->queue + 32, (u64)(uptr)v->status_byte, 1, VRING_WRITE, 0);

    w32(io + VIO_QPFN, (u32)(pages >> 12));
    u32 lo = r32(io + VIO_CONFIG);
    u32 hi = r32(io + VIO_CONFIG + 4);
    v->sectors = ((u64)hi << 32) | lo;
    if (!v->sectors) {
        efi_free_pages(pages, 4);
        return 0;
    }
    w8(io + VIO_STATUS, VIO_ACK | VIO_DRIVER | VIO_DRIVER_OK);
    return 1;
}

static int vblk_xfer(struct vblk *v, u64 lba, void *buf, u32 bytes, int write)
{
    if (!bytes || (bytes & 511) || !buf)
        return 0;
    /* Header: type, reserved, sector */
    memset(v->hdr, 0, 16);
    u32 type = write ? VBLK_T_OUT : VBLK_T_IN;
    memcpy(v->hdr, &type, 4);
    memcpy(v->hdr + 8, &lba, 8);
    *v->status_byte = 0xFF;

    u16 flags = VRING_NEXT | (write ? 0 : VRING_WRITE);
    desc_set(v->queue + 16, (u64)(uptr)buf, bytes, flags, 2);

    u8 *avail = v->queue + v->avail_off;
    u16 idx;
    memcpy(&idx, avail + 2, 2);
    u16 slot = idx & (v->qsize - 1);
    u16 zero = 0;
    memcpy(avail + 4 + slot * 2, &zero, 2);
    idx++;
    memcpy(avail + 2, &idx, 2);

    w16(v->io + VIO_QNOTIFY, 0);

    u8 *used = v->queue + v->used_off;
    for (int i = 0; i < 1000000; i++) {
        u16 ui;
        memcpy(&ui, used + 2, 2);
        if (ui != v->last_used) {
            v->last_used = ui;
            return *v->status_byte == 0;
        }
    }
    return 0;
}

static int vblk_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return vblk_xfer((struct vblk *)b, lba, buf, bytes, 0);
}

static int vblk_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return vblk_xfer((struct vblk *)b, lba, buf, bytes, 1);
}

/* Find the first transitional virtio-blk with an I/O BAR0, bring it up
 * and publish it (efi_disk_add) */
int vblk_probe(void)
{
    u8 bus, dev, fn;
    if (!pci_find(VBLK_VENDOR, VBLK_DEVICE, &bus, &dev, &fn))
        return 0;
    u32 bar0 = pci_read32(bus, dev, fn, 0x10);
    if (!(bar0 & 1))
        return 0;                                       /* need legacy I/O BAR */
    u16 io = pci_assign_io_bar(bus, dev, fn, 0x10);
    if (!io)
        return 0;
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, cmd | 0x05);       /* I/O + bus master */
    if (!vblk_init(&disk, io, bus, dev, fn))
        return 0;
    printf("disk: virtio-blk at PCI %02x:%02x.%x, %lu sectors\n",
           bus, dev, fn, disk.sectors);
    disk.blk.sectors = disk.sectors;
    disk.blk.path = efi_dp_pci(dev, fn);
    disk.blk.read = vblk_read;
    disk.blk.write = vblk_write;
    efi_disk_add(&disk.blk);
    return 1;
}
