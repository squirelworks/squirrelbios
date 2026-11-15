/* SquirrelBIOS UEFI core (common/efi): virtqueues and virtio-blk, for any
 * transport (virtio_mmio.c, virtio_pci.c). Original code, from the VIRTIO
 * 1.1 specification (2.6 split virtqueues, 5.2 block device).
 *
 * One queue of VQ_SIZE descriptors per disk, one request at a time on
 * descriptors 0-2 (header, data, status), polled. The layout suits both
 * the legacy MMIO interface (used ring 4 KB-aligned) and the modern ones
 * (three addresses). Addresses are physical: the firmware runs
 * identity-mapped or without paging. */
#include "efi.h"

#define DESC_NEXT   1
#define DESC_WRITE  2
#define BLK_T_IN    0
#define BLK_T_OUT   1
#define CHUNK       (1u << 20)

struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

/* Three pages: the descriptors and the avail ring, the used ring, then
 * the request header and status */
int vq_alloc(struct vq *q)
{
    EFI_PHYSICAL_ADDRESS a = 0;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, 3, &a) != EFI_SUCCESS)
        return 0;
    q->mem = (u8 *)(uptr)a;
    memset(q->mem, 0, 3 * 4096);
    q->req = q->mem + 2 * 4096;
    q->last_used = 0;
    return 1;
}

uptr vq_desc(struct vq *q) { return (uptr)q->mem; }
uptr vq_avail(struct vq *q) { return (uptr)q->mem + VQ_SIZE * sizeof(struct vring_desc); }
uptr vq_used(struct vq *q) { return (uptr)q->mem + VQ_USED_OFF; }

static void desc(struct vq *q, int i, u64 addr, u32 len, u16 flags, u16 next)
{
    struct vring_desc *d = (struct vring_desc *)q->mem + i;
    d->addr = addr;
    d->len = len;
    d->flags = flags;
    d->next = next;
}

static int request(struct vq *q, u32 type, u64 lba, void *buf, u32 bytes)
{
    u8 *h = q->req;
    memset(h, 0, 16);
    memcpy(h, &type, 4);
    memcpy(h + 8, &lba, 8);
    h[16] = 0xFF;
    desc(q, 0, (uptr)h, 16, DESC_NEXT, 1);
    desc(q, 1, (uptr)buf, bytes, DESC_NEXT | (type == BLK_T_IN ? DESC_WRITE : 0), 2);
    desc(q, 2, (uptr)(h + 16), 1, DESC_WRITE, 0);

    volatile u16 *avail = (volatile u16 *)vq_avail(q);
    volatile u16 *used_idx = (volatile u16 *)(vq_used(q) + 2);
    u16 idx = avail[1];
    avail[2 + idx % VQ_SIZE] = 0;
    __sync_synchronize();
    avail[1] = (u16)(idx + 1);
    __sync_synchronize();
    if (q->notify16)
        mmio_write16(q->notify, 0);
    else
        mmio_write32(q->notify, 0);

    for (u64 spins = 0; *used_idx == q->last_used; spins++) {
        if (spins > 2000000000ULL)
            return 0;
        __sync_synchronize();
    }
    q->last_used++;
    __sync_synchronize();
    return *(volatile u8 *)(h + 16) == 0;
}

struct vblk {
    struct sq_blk blk;          /* first */
    struct vq *q;
};

static int vblk_rw(struct sq_blk *b, u32 type, u64 lba, u8 *buf, u32 bytes)
{
    struct vq *q = ((struct vblk *)b)->q;
    while (bytes) {
        u32 n = bytes > CHUNK ? CHUNK : bytes;
        if (!request(q, type, lba, buf, n))
            return 0;
        lba += n / 512;
        buf += n;
        bytes -= n;
    }
    return 1;
}

static int vblk_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return vblk_rw(b, BLK_T_IN, lba, buf, bytes);
}

static int vblk_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return vblk_rw(b, BLK_T_OUT, lba, buf, bytes);
}

/* A transport set up queue 0 of a block device (and printed where it
 * is): publish the disk */
int virtio_blk_add(struct vq *q, u64 sectors, EFI_DEVICE_PATH_PROTOCOL *path)
{
    struct vblk *v;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *v, (void **)&v) != EFI_SUCCESS)
        return 0;
    memset(v, 0, sizeof *v);
    v->q = q;
    v->blk.sectors = sectors;
    v->blk.path = path;
    v->blk.read = vblk_read;
    v->blk.write = vblk_write;
    efi_disk_add(&v->blk);
    return 1;
}
