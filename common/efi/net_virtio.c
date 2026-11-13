/* SquirrelBIOS UEFI: virtio-net, polled, over any transport. Original code.
 *
 * Written from the Virtio 1.0 specification (legacy interface notes for
 * the older transports). Queue 0 receives, queue 1 transmits. Every
 * receive buffer is one descriptor of NET_RXBUF bytes, the virtio-net
 * header and the frame together; the transmit queue uses descriptor 0
 * for one frame at a time and waits for it. The only feature taken is
 * VIRTIO_NET_F_MAC (and VIRTIO_F_VERSION_1 where the transport offers
 * it, which makes the header 12 bytes instead of 10). The rings are laid
 * out as the legacy interface wants them (used ring on the next 4 KB
 * page), which suits the modern one too. */
#include "net.h"

#define VQ_ALIGN        4096
#define NET_RXBUFS      32
#define NET_RXBUF       2048
#define F_MAC           5
#define S_ACK           1
#define S_DRIVER        2
#define S_DRIVER_OK     4
#define S_FEATURES_OK   8
#define DESC_WRITE      2

struct nvq {
    u16 size;
    u16 last_used;
    u16 avail_idx;
    volatile u8 *desc;          /* 16 bytes each: addr, len, flags, next */
    volatile u16 *avail;        /* flags, idx, ring[size] */
    volatile u8 *used;          /* flags, idx, then {id, len} each */
};

struct vnet {
    struct net_dev nd;
    struct vnet_transport *t;
    struct nvq rx, tx;
    u32 hdr;                    /* virtio-net header bytes */
    u8 *rxbufs;
    u8 *txbuf;
};

static void barrier(void)
{
    __sync_synchronize();
}

static u16 used_idx(struct nvq *q)
{
    barrier();
    return *(volatile u16 *)(q->used + 2);
}

static int nvq_setup(struct vnet_transport *t, u16 index, struct nvq *q)
{
    u16 n = t->queue_max(t->ctx, index);
    if (!n)
        return 0;
    if (n > 256)
        n = 256;                /* the legacy interface takes the device's size */
    u64 used_off = ((u64)n * 16 + 6 + 2 * (u64)n + VQ_ALIGN - 1) & ~(u64)(VQ_ALIGN - 1);
    u64 bytes = used_off + 6 + 8 * (u64)n;
    EFI_PHYSICAL_ADDRESS pa;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, (bytes + 4095) / 4096, &pa) != EFI_SUCCESS)
        return 0;
    memset((void *)(uptr)pa, 0, (bytes + 4095) & ~4095ull);
    q->size = n;
    q->desc = (volatile u8 *)(uptr)pa;
    q->avail = (volatile u16 *)(uptr)(pa + (u64)n * 16);
    q->used = (volatile u8 *)(uptr)(pa + used_off);
    q->last_used = q->avail_idx = 0;
    return t->queue_set(t->ctx, index, n, pa, pa + (u64)n * 16, pa + used_off);
}

static void desc_set(struct nvq *q, u16 i, u64 addr, u32 len, u16 flags)
{
    volatile u8 *d = q->desc + 16 * (u32)i;
    *(volatile u64 *)d = addr;
    *(volatile u32 *)(d + 8) = len;
    *(volatile u16 *)(d + 12) = flags;
    *(volatile u16 *)(d + 14) = 0;
}

static void avail_put(struct nvq *q, u16 id)
{
    q->avail[2 + q->avail_idx % q->size] = id;
    q->avail_idx++;
    barrier();
    q->avail[1] = q->avail_idx;
    barrier();
}

static int vnet_send(struct net_dev *nd, const void *frame, u32 len)
{
    struct vnet *v = (struct vnet *)nd;
    if (len > NET_RXBUF - v->hdr)
        return -1;
    memset(v->txbuf, 0, v->hdr);
    memcpy(v->txbuf + v->hdr, frame, len);
    desc_set(&v->tx, 0, (u64)(uptr)v->txbuf, v->hdr + len, 0);
    u16 want = v->tx.last_used + 1;
    avail_put(&v->tx, 0);
    v->t->notify(v->t->ctx, 1);
    u64 end = net_ms() + 200;
    while (used_idx(&v->tx) != want) {
        if (net_ms() > end)
            return -1;
    }
    v->tx.last_used = want;
    return 0;
}

static int vnet_recv(struct net_dev *nd, void *buf, u32 max)
{
    struct vnet *v = (struct vnet *)nd;
    if (used_idx(&v->rx) == v->rx.last_used)
        return 0;
    volatile u8 *e = v->rx.used + 4 + 8 * (u32)(v->rx.last_used % v->rx.size);
    u16 id = (u16)*(volatile u32 *)e;
    u32 len = *(volatile u32 *)(e + 4);
    v->rx.last_used++;
    int got = 0;
    if (id < NET_RXBUFS && len > v->hdr) {
        len -= v->hdr;
        if (len > max)
            len = max;
        memcpy(buf, v->rxbufs + (u32)id * NET_RXBUF + v->hdr, len);
        got = (int)len;
    }
    if (id < NET_RXBUFS) {      /* the buffer goes back on the ring */
        avail_put(&v->rx, id);
        v->t->notify(v->t->ctx, 0);
    }
    return got;
}

static void vnet_stop(struct net_dev *nd)
{
    struct vnet *v = (struct vnet *)nd;
    v->t->set_status(v->t->ctx, 0);     /* a reset: no more DMA */
}

struct net_dev *vnet_init(struct vnet_transport *t)
{
    struct vnet *v;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(*v), (void **)&v) != EFI_SUCCESS)
        return NULL;
    memset(v, 0, sizeof(*v));
    v->t = t;
    t->set_status(t->ctx, 0);
    t->set_status(t->ctx, S_ACK);
    t->set_status(t->ctx, S_ACK | S_DRIVER);
    u32 f0 = t->features(t->ctx, 0);
    u32 want0 = f0 & (1u << F_MAC);
    t->set_features(t->ctx, 0, want0);
    u8 st = S_ACK | S_DRIVER;
    if (t->version1) {
        t->set_features(t->ctx, 1, 1);  /* VIRTIO_F_VERSION_1 */
        st |= S_FEATURES_OK;
        t->set_status(t->ctx, st);
        if (!(t->get_status(t->ctx) & S_FEATURES_OK))
            goto fail;
    }
    v->hdr = t->version1 ? 12 : 10;
    for (int i = 0; i < 6; i++)
        v->nd.mac[i] = want0 ? t->config8(t->ctx, (u32)i) : (u8)(i ? 0x10 * i : 0x02);
    if (!nvq_setup(t, 0, &v->rx) || !nvq_setup(t, 1, &v->tx))
        goto fail;
    EFI_PHYSICAL_ADDRESS pa;
    u32 pages = (NET_RXBUFS * NET_RXBUF + NET_RXBUF + 4095) / 4096;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, pages, &pa) != EFI_SUCCESS)
        goto fail;
    v->rxbufs = (u8 *)(uptr)pa;
    v->txbuf = v->rxbufs + NET_RXBUFS * NET_RXBUF;
    u16 nrx = v->rx.size < NET_RXBUFS ? v->rx.size : NET_RXBUFS;
    for (u16 i = 0; i < nrx; i++) {
        desc_set(&v->rx, i, (u64)(uptr)(v->rxbufs + (u32)i * NET_RXBUF), NET_RXBUF, DESC_WRITE);
        avail_put(&v->rx, i);
    }
    t->set_status(t->ctx, st | S_DRIVER_OK);
    t->notify(t->ctx, 0);
    v->nd.mtu = 1500;
    v->nd.send = vnet_send;
    v->nd.recv = vnet_recv;
    v->nd.stop = vnet_stop;
    v->nd.priv = v;
    return &v->nd;
fail:
    t->set_status(t->ctx, 0x80);        /* FAILED */
    efi_free_pool(v);
    return NULL;
}
