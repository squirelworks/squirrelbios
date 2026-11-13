/* SquirrelBIOS x86_64-uefi: the platform side of the network stack in
 * common/efi/net_*.c. Original code.
 *
 * virtio-net on PCI through the legacy interface (I/O BAR0, VIRTIO 1.1
 * section 4.1.4.8): every transitional virtio-net (1af4:1000) on bus 0 is
 * handed to net_virtio.c. MSI-X stays off, so the MAC address sits at
 * offset 20 of the BAR. Time comes from the TSC. */
#include "../../common/efi/net.h"

#define VNET_VENDOR 0x1AF4
#define VNET_DEVICE 0x1000

#define VIO_HOST_F   0x00
#define VIO_GUEST_F  0x04
#define VIO_QPFN     0x08
#define VIO_QSIZE    0x0C
#define VIO_QSEL     0x0E
#define VIO_QNOTIFY  0x10
#define VIO_STATUS   0x12
#define VIO_CONFIG   0x14

const u16 net_client_arch = 7;      /* x64 UEFI */

u64 net_ms(void)
{
    static u64 per_ms;
    if (!per_ms)
        per_ms = efi_tsc_hz() / 1000 ? efi_tsc_hz() / 1000 : 1000000;
    return rdtsc() / per_ms;
}

struct vio_pci {
    struct vnet_transport t;
    u16 io;
};

static u16 io_of(void *ctx) { return ((struct vio_pci *)ctx)->io; }

static u32 lp_features(void *ctx, int word)
{
    return word ? 0 : inl(io_of(ctx) + VIO_HOST_F);
}

static void lp_set_features(void *ctx, int word, u32 v)
{
    if (!word)
        outl(io_of(ctx) + VIO_GUEST_F, v);
}

static u8 lp_get_status(void *ctx) { return inb(io_of(ctx) + VIO_STATUS); }
static void lp_set_status(void *ctx, u8 s) { outb(io_of(ctx) + VIO_STATUS, s); }
static u8 lp_config8(void *ctx, u32 off) { return inb((u16)(io_of(ctx) + VIO_CONFIG + off)); }

static u16 lp_queue_max(void *ctx, u16 q)
{
    outw(io_of(ctx) + VIO_QSEL, q);
    return inw(io_of(ctx) + VIO_QSIZE);
}

/* The legacy interface: the device's own queue size, one block from desc
 * with the used ring on a 4 KB boundary, given as a page number */
static int lp_queue_set(void *ctx, u16 q, u16 size, u64 desc, u64 avail, u64 used)
{
    (void)avail;
    (void)used;
    outw(io_of(ctx) + VIO_QSEL, q);
    if (inw(io_of(ctx) + VIO_QSIZE) != size || (desc & 4095) || (desc >> 44))
        return 0;
    outl(io_of(ctx) + VIO_QPFN, (u32)(desc >> 12));
    return 1;
}

static void lp_notify(void *ctx, u16 q) { outw(io_of(ctx) + VIO_QNOTIFY, q); }

static void probe_one(u8 bus, u8 dev, u8 fn)
{
    if (!(pci_read32(bus, dev, fn, 0x10) & 1))
        return;                 /* no legacy I/O BAR */
    u16 io = pci_assign_io_bar(bus, dev, fn, 0x10);
    if (!io)
        return;
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, cmd | 0x05);       /* I/O + bus master */
    struct vio_pci *v;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *v, (void **)&v) != EFI_SUCCESS)
        return;
    memset(v, 0, sizeof *v);
    v->io = io;
    v->t.ctx = v;
    v->t.version1 = 0;
    v->t.features = lp_features;
    v->t.set_features = lp_set_features;
    v->t.get_status = lp_get_status;
    v->t.set_status = lp_set_status;
    v->t.config8 = lp_config8;
    v->t.queue_max = lp_queue_max;
    v->t.queue_set = lp_queue_set;
    v->t.notify = lp_notify;
    struct net_dev *nd = vnet_init(&v->t);
    if (!nd) {
        printf("net: virtio-net at PCI %02x:%02x.%x did not start\n", bus, dev, fn);
        efi_free_pool(v);
        return;
    }
    char what[40] = "virtio-net at PCI 00:00.0";
    const char *hex = "0123456789abcdef";
    what[18] = hex[bus >> 4];
    what[19] = hex[bus & 15];
    what[21] = hex[dev >> 4];
    what[22] = hex[dev & 15];
    what[24] = hex[fn & 7];
    net_add(nd, bus == 0 ? efi_dp_pci(dev, fn) : NULL, what);
}

int net_platform_probe(void)
{
    int n = 0;
    for (u8 d = 0; d < 32; d++) {
        if ((pci_read32(0, d, 0, 0) & 0xFFFF) == 0xFFFF)
            continue;
        u8 maxfn = (pci_read8(0, d, 0, 0x0E) & 0x80) ? 8 : 1;
        for (u8 f = 0; f < maxfn; f++) {
            u32 id = pci_read32(0, d, f, 0);
            if ((id & 0xFFFF) == VNET_VENDOR && (id >> 16) == VNET_DEVICE) {
                probe_one(0, d, f);
                n++;
            }
        }
    }
    return n;
}
