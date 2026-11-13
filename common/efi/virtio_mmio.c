/* SquirrelBIOS UEFI core (common/efi): virtio-blk on virtio-mmio.
 * Original code, from the VIRTIO 1.1 specification (section 4.2, MMIO
 * transport, with its legacy interface in 4.2.4). The device tree lists
 * the transports ("virtio,mmio"); every one with a block device behind
 * it is a disk. The queue and requests are virtio.c's. Both the version 1
 * (legacy) and version 2 register layouts work: QEMU uses the legacy
 * one unless -global virtio-mmio.force-legacy=false.
 */
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

/* Vendor GUID of a disk's device path node: hardware vendor, then the
 * transport's MMIO base (8 bytes) */
#define SQ_VIRTIO_MMIO_GUID \
    { 0x5371b1d3, 0x6d2c, 0x4f0e, { 0x9a, 0x38, 0x51, 0x75, 0x69, 0x72, 0x74, 0x6d } }

static uptr base;

static u32 rd(u32 off) { return mmio_read32(base + off); }
static void wr(u32 off, u32 v) { mmio_write32(base + off, v); }

static void wr64(u32 off, u64 v)
{
    wr(off, (u32)v);
    wr(off + 4, (u32)(v >> 32));
}

/* Hardware vendor node (type 1, subtype 4): GUID, MMIO base */
static EFI_DEVICE_PATH_PROTOCOL *mmio_path(u64 b)
{
    static const EFI_GUID g = SQ_VIRTIO_MMIO_GUID;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, 28 + 4, &mem) != EFI_SUCCESS)
        return NULL;
    u8 *p = mem;
    p[0] = HARDWARE_DEVICE_PATH;
    p[1] = 4;
    p[2] = 28;
    p[3] = 0;
    memcpy(p + 4, &g, 16);
    memcpy(p + 20, &b, 8);
    p[28] = END_DEVICE_PATH_TYPE;
    p[29] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[30] = 4;
    p[31] = 0;
    return mem;
}

/* The transport at b: a block device set up and published, or 0 */
static int setup(uptr b)
{
    base = b;
    if (rd(VM_MAGIC) != VIRT_MAGIC || rd(VM_DEVICE_ID) != DEVICE_BLOCK)
        return 0;
    u32 version = rd(VM_VERSION);
    if (version != 1 && version != 2)
        return 0;

    wr(VM_STATUS, 0);
    wr(VM_STATUS, ST_ACK);
    wr(VM_STATUS, ST_ACK | ST_DRIVER);
    u32 st = ST_ACK | ST_DRIVER;
    if (version == 2) {
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
    if (rd(VM_QUEUE_MAX) < VQ_SIZE)
        goto fail;
    struct vq *q;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *q, (void **)&q) != EFI_SUCCESS || !vq_alloc(q))
        goto fail;
    q->notify = b + VM_QUEUE_NOTIFY;
    q->notify16 = 0;
    wr(VM_QUEUE_NUM, VQ_SIZE);
    if (version == 2) {
        wr64(VM_QUEUE_DESC, vq_desc(q));
        wr64(VM_QUEUE_DRIVER, vq_avail(q));
        wr64(VM_QUEUE_DEVICE, vq_used(q));
        wr(VM_QUEUE_READY, 1);
    } else {
        wr(VM_QUEUE_ALIGN, VQ_USED_OFF);
        wr(VM_QUEUE_PFN, (u32)(vq_desc(q) >> 12));
    }
    wr(VM_STATUS, st | ST_DRIVER_OK);

    u64 sectors = (u64)rd(VM_CONFIG) | (u64)rd(VM_CONFIG + 4) << 32;
    printf("disk: virtio-blk at 0x%016lx (virtio-mmio v%u), %lu sectors\n",
           (u64)b, version, sectors);
    return virtio_blk_add(q, sectors, mmio_path(b));
fail:
    wr(VM_STATUS, ST_FAILED);
    return 0;
}

/* Every "virtio,mmio" transport in the device tree with a disk behind it */
int virtio_mmio_probe(void)
{
    int n = 0;
    for (int node = fdt_find_compatible("virtio,mmio", -1); node >= 0;
         node = fdt_find_compatible("virtio,mmio", node)) {
        u64 b, size;
        if (fdt_reg(node, 0, &b, &size))
            n += setup((uptr)b);
    }
    return n;
}
