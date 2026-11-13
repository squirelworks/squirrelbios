/* SquirrelBIOS UEFI core (common/efi): virtio-blk on PCI (the modern
 * interface). Original code, from the VIRTIO 1.1 specification (section
 * 4.1, PCI transport: vendor capabilities, common configuration,
 * notifications). Transitional (0x1001) and modern (0x1042) block devices
 * both carry the capabilities; the queue and requests are virtio.c's. */
#include "efi.h"

#define DEV_BLK_LEGACY   0x1001
#define DEV_BLK_MODERN   0x1042

#define CFG_STATUS       0x06
#define CFG_CAP_PTR      0x34
#define STATUS_CAPS      0x10
#define CAP_VENDOR       0x09

#define CAP_COMMON       1
#define CAP_NOTIFY       2
#define CAP_DEVICE       4

/* Common configuration */
#define CC_DFSELECT      0x00
#define CC_DF            0x04
#define CC_GFSELECT      0x08
#define CC_GF            0x0C
#define CC_STATUS        0x14
#define CC_Q_SELECT      0x16
#define CC_Q_SIZE        0x18
#define CC_Q_ENABLE      0x1C
#define CC_Q_NOTIFY_OFF  0x1E
#define CC_Q_DESC        0x20
#define CC_Q_DRIVER      0x28
#define CC_Q_DEVICE      0x30

#define ST_ACK           0x01
#define ST_DRIVER        0x02
#define ST_DRIVER_OK     0x04
#define ST_FEATURES_OK   0x08
#define ST_FAILED        0x80

#define F_VERSION_1      (1u << 0)  /* feature bit 32, in the second word */

static void w64(uptr a, u64 v)
{
    mmio_write32(a, (u32)v);
    mmio_write32(a + 4, (u32)(v >> 32));
}

int virtio_pci_add(struct sq_pci *p)
{
    if (p->device != DEV_BLK_LEGACY && p->device != DEV_BLK_MODERN)
        return 0;
    if (!(mmio_read16(p->cfg + CFG_STATUS) & STATUS_CAPS))
        return 0;

    uptr common = 0, notify = 0, device = 0;
    u32 mult = 0;
    u8 at = mmio_read8(p->cfg + CFG_CAP_PTR) & ~3;
    for (int guard = 0; at && guard < 48; guard++) {
        uptr c = p->cfg + at;
        if (mmio_read8(c) == CAP_VENDOR) {
            u8 type = mmio_read8(c + 3), b = mmio_read8(c + 4);
            uptr where = b < 6 && p->bar[b] ? (uptr)p->bar[b] + mmio_read32(c + 8) : 0;
            if (type == CAP_COMMON && !common)
                common = where;
            else if (type == CAP_NOTIFY && !notify) {
                notify = where;
                mult = mmio_read32(c + 16);
            } else if (type == CAP_DEVICE && !device)
                device = where;
        }
        at = mmio_read8(c + 1) & ~3;
    }
    if (!common || !notify || !device)
        return 0;

    mmio_write8(common + CC_STATUS, 0);
    mmio_write8(common + CC_STATUS, ST_ACK);
    u8 st = ST_ACK | ST_DRIVER;
    mmio_write8(common + CC_STATUS, st);
    mmio_write32(common + CC_DFSELECT, 1);
    if (!(mmio_read32(common + CC_DF) & F_VERSION_1))
        goto fail;
    mmio_write32(common + CC_GFSELECT, 0);
    mmio_write32(common + CC_GF, 0);
    mmio_write32(common + CC_GFSELECT, 1);
    mmio_write32(common + CC_GF, F_VERSION_1);
    st |= ST_FEATURES_OK;
    mmio_write8(common + CC_STATUS, st);
    if (!(mmio_read8(common + CC_STATUS) & ST_FEATURES_OK))
        goto fail;

    mmio_write16(common + CC_Q_SELECT, 0);
    if (mmio_read16(common + CC_Q_SIZE) < VQ_SIZE)
        goto fail;
    struct vq *q;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *q, (void **)&q) != EFI_SUCCESS || !vq_alloc(q))
        goto fail;
    mmio_write16(common + CC_Q_SIZE, VQ_SIZE);
    w64(common + CC_Q_DESC, vq_desc(q));
    w64(common + CC_Q_DRIVER, vq_avail(q));
    w64(common + CC_Q_DEVICE, vq_used(q));
    q->notify = notify + (uptr)mmio_read16(common + CC_Q_NOTIFY_OFF) * mult;
    q->notify16 = 1;
    mmio_write16(common + CC_Q_ENABLE, 1);
    mmio_write8(common + CC_STATUS, st | ST_DRIVER_OK);

    u64 sectors = (u64)mmio_read32(device) | (u64)mmio_read32(device + 4) << 32;
    printf("disk: virtio-blk at PCI 00:%02x.%x (virtio-pci), %lu sectors\n",
           p->dev, p->fn, sectors);
    return virtio_blk_add(q, sectors, p->path);
fail:
    mmio_write8(common + CC_STATUS, ST_FAILED);
    return 0;
}
