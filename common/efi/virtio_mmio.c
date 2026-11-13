/* SquirrelBIOS UEFI core (common/efi): virtio-blk on virtio-mmio.
 * Original code, from the VIRTIO 1.1 specification (section 4.2, MMIO
 * transport, with its legacy interface in 4.2.4). The device tree lists
 * the transports ("virtio,mmio"); the first one with a block device
 * behind it is the disk. Both the version 1 (legacy) and version 2
 * register layouts work: QEMU uses the legacy one unless
 * -global virtio-mmio.force-legacy=false.
 *
 * One request at a time on descriptors 0-2, polled. Addresses are
 * physical: the firmware runs identity-mapped (or with no paging). */
#include "efi.h"

#define VM_MAGIC         0x000
#define VM_VERSION       0x004
#define VM_DEVICE_ID     0x008
#define VM_DEV_FEAT      0x010
#define VM_DEV_FEAT_SEL  0x014
#define VM_DRV_FEAT      0x020
#define VM_DRV_FEAT_SEL  0x024
#define VM_GUEST_PAGE    0x028      /* legacy */
#define VM_QUEUE_SEL     0x030
#define VM_QUEUE_MAX     0x034
#define VM_QUEUE_NUM     0x038
#define VM_QUEUE_ALIGN   0x03c      /* legacy */
#define VM_QUEUE_PFN     0x040      /* legacy */
#define VM_QUEUE_READY   0x044
#define VM_QUEUE_NOTIFY  0x050
#define VM_INT_STATUS    0x060
#define VM_INT_ACK       0x064
#define VM_STATUS        0x070
#define VM_QUEUE_DESC    0x080
#define VM_QUEUE_DRIVER  0x090
#define VM_QUEUE_DEVICE  0x0a0
#define VM_CONFIG        0x100

#define VIRT_MAGIC       0x74726976 /* "virt" */
#define DEVICE_BLOCK     2

#define ST_ACK           0x01
#define ST_DRIVER        0x02
#define ST_DRIVER_OK     0x04
#define ST_FEATURES_OK   0x08
#define ST_FAILED        0x80

#define F_VERSION_1      (1u << 0)  /* feature bit 32, in the second word */

#define QSIZE            8
#define USED_OFF         4096       /* the legacy layout: used ring page-aligned */
#define DESC_NEXT        1
#define DESC_WRITE       2
#define BLK_T_IN         0
#define BLK_T_OUT        1
#define CHUNK            (1u << 20)

/* Vendor GUID of the disk's device path node: hardware vendor, then the
 * transport's MMIO base (8 bytes) */
#define SQ_VIRTIO_MMIO_GUID \
    { 0x5371b1d3, 0x6d2c, 0x4f0e, { 0x9a, 0x38, 0x51, 0x75, 0x69, 0x72, 0x74, 0x6d } }

struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

static struct {
    uptr base;
    u32 version;
    u64 sectors;
    u8 *queue;                  /* two pages: descriptors + avail, used */
    u16 last_used;
    u8 *req;                    /* 16-byte header, then the status byte */
} vb;
static int have_disk;

static u32 rd(u32 off) { return mmio_read32(vb.base + off); }
static void wr(u32 off, u32 v) { mmio_write32(vb.base + off, v); }

static void wr64(u32 off, u64 v)
{
    wr(off, (u32)v);
    wr(off + 4, (u32)(v >> 32));
}

static int vblk_setup(uptr base)
{
    vb.base = base;
    if (rd(VM_MAGIC) != VIRT_MAGIC || rd(VM_DEVICE_ID) != DEVICE_BLOCK)
        return 0;
    vb.version = rd(VM_VERSION);
    if (vb.version != 1 && vb.version != 2)
        return 0;

    wr(VM_STATUS, 0);
    wr(VM_STATUS, ST_ACK);
    wr(VM_STATUS, ST_ACK | ST_DRIVER);
    u32 st = ST_ACK | ST_DRIVER;
    if (vb.version == 2) {
        wr(VM_DEV_FEAT_SEL, 1);
        if (!(rd(VM_DEV_FEAT) & F_VERSION_1))
            goto fail;
        wr(VM_DRV_FEAT_SEL, 0);
        wr(VM_DRV_FEAT, 0);
        wr(VM_DRV_FEAT_SEL, 1);
        wr(VM_DRV_FEAT, F_VERSION_1);
        st |= ST_FEATURES_OK;
        wr(VM_STATUS, st);
        if (!(rd(VM_STATUS) & ST_FEATURES_OK))
            goto fail;
    } else {
        wr(VM_DRV_FEAT_SEL, 0);
        wr(VM_DRV_FEAT, 0);
        wr(VM_GUEST_PAGE, 4096);
    }

    wr(VM_QUEUE_SEL, 0);
    u32 max = rd(VM_QUEUE_MAX);
    if (max < QSIZE)
        goto fail;
    if (!vb.queue) {
        EFI_PHYSICAL_ADDRESS a = 0;
        if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, 3, &a) != EFI_SUCCESS)
            goto fail;
        vb.queue = (u8 *)(uptr)a;
        vb.req = vb.queue + 2 * 4096;
    }
    memset(vb.queue, 0, 2 * 4096);
    vb.last_used = 0;
    wr(VM_QUEUE_NUM, QSIZE);
    uptr q = (uptr)vb.queue;
    if (vb.version == 2) {
        wr64(VM_QUEUE_DESC, q);
        wr64(VM_QUEUE_DRIVER, q + QSIZE * sizeof(struct vring_desc));
        wr64(VM_QUEUE_DEVICE, q + USED_OFF);
        wr(VM_QUEUE_READY, 1);
    } else {
        wr(VM_QUEUE_ALIGN, USED_OFF);
        wr(VM_QUEUE_PFN, (u32)(q >> 12));
    }
    wr(VM_STATUS, st | ST_DRIVER_OK);

    vb.sectors = (u64)rd(VM_CONFIG) | (u64)rd(VM_CONFIG + 4) << 32;
    return 1;
fail:
    wr(VM_STATUS, ST_FAILED);
    return 0;
}

int vblk_probe(void)
{
    if (have_disk)
        return 1;
    for (int node = fdt_find_compatible("virtio,mmio", -1); node >= 0;
         node = fdt_find_compatible("virtio,mmio", node)) {
        u64 base, size;
        if (!fdt_reg(node, 0, &base, &size))
            continue;
        if (vblk_setup((uptr)base)) {
            have_disk = 1;
            printf("disk: virtio-blk at 0x%016lx (virtio-mmio v%u), %lu sectors\n", base,
                   vb.version, vb.sectors);
            return 1;
        }
    }
    return 0;
}

int vblk_present(void) { return have_disk; }
u64 vblk_sectors(void) { return vb.sectors; }

/* Hardware vendor node (type 1, subtype 4): GUID, MMIO base */
EFI_DEVICE_PATH_PROTOCOL *vblk_path(void)
{
    static const EFI_GUID g = SQ_VIRTIO_MMIO_GUID;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, 28 + 4, &mem) != EFI_SUCCESS)
        return NULL;
    u8 *p = mem;
    u64 base = vb.base;
    p[0] = HARDWARE_DEVICE_PATH;
    p[1] = 4;
    p[2] = 28;
    p[3] = 0;
    memcpy(p + 4, &g, 16);
    memcpy(p + 20, &base, 8);
    p[28] = END_DEVICE_PATH_TYPE;
    p[29] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[30] = 4;
    p[31] = 0;
    return mem;
}

static void desc(int i, u64 addr, u32 len, u16 flags, u16 next)
{
    struct vring_desc *d = (struct vring_desc *)vb.queue + i;
    d->addr = addr;
    d->len = len;
    d->flags = flags;
    d->next = next;
}

static int request(u32 type, u64 lba, void *buf, u32 bytes)
{
    u8 *h = vb.req;
    memset(h, 0, 16);
    memcpy(h, &type, 4);
    memcpy(h + 8, &lba, 8);
    h[16] = 0xFF;
    desc(0, (uptr)h, 16, DESC_NEXT, 1);
    desc(1, (uptr)buf, bytes, DESC_NEXT | (type == BLK_T_IN ? DESC_WRITE : 0), 2);
    desc(2, (uptr)(h + 16), 1, DESC_WRITE, 0);

    volatile u16 *avail = (volatile u16 *)(vb.queue + QSIZE * sizeof(struct vring_desc));
    volatile u16 *used_idx = (volatile u16 *)(vb.queue + USED_OFF + 2);
    u16 idx = avail[1];
    avail[2 + idx % QSIZE] = 0;
    __sync_synchronize();
    avail[1] = (u16)(idx + 1);
    __sync_synchronize();
    wr(VM_QUEUE_NOTIFY, 0);

    for (u64 spins = 0; *used_idx == vb.last_used; spins++) {
        if (spins > 2000000000ULL)
            return 0;
        __sync_synchronize();
    }
    vb.last_used++;
    wr(VM_INT_ACK, rd(VM_INT_STATUS));
    __sync_synchronize();
    return *(volatile u8 *)(h + 16) == 0;
}

static int vblk_rw(u32 type, u64 lba, u8 *buf, u32 bytes)
{
    if (!have_disk)
        return 0;
    while (bytes) {
        u32 n = bytes > CHUNK ? CHUNK : bytes;
        if (!request(type, lba, buf, n))
            return 0;
        lba += n / 512;
        buf += n;
        bytes -= n;
    }
    return 1;
}

int vblk_read(u64 lba, void *buf, u32 bytes) { return vblk_rw(BLK_T_IN, lba, buf, bytes); }
int vblk_write(u64 lba, void *buf, u32 bytes) { return vblk_rw(BLK_T_OUT, lba, buf, bytes); }
