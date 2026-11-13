/* SquirrelBIOS UEFI core (common/efi): USB mass storage on xHCI.
 * Original code, from the eXtensible Host Controller Interface 1.2
 * specification (registers, rings, contexts, commands), USB 2.0 chapter 9
 * (standard requests and descriptors), the USB Mass Storage Class
 * Bulk-Only Transport 1.0 and SCSI SBC/SPC (READ CAPACITY, READ/WRITE).
 *
 * One controller pass: the controller is reset, given a command ring and
 * one event ring (polled, no interrupts), then every root port with a
 * device is reset and the device addressed. A bulk-only SCSI disk with
 * 512-byte blocks on a root port becomes a disk; other devices are left
 * addressed but unconfigured. No hubs, no streams (UAS). Data goes
 * through a page-aligned bounce buffer of BOUNCE bytes. */
#include "efi.h"

#define BOUNCE      (64u * 1024)
#define RING_TRBS   256                 /* one page; the last is the link */
#define SPINS       200000000ULL
#define MAX_SLOTS   16

/* Capability registers */
#define CAP_LENGTH  0x00
#define CAP_HCS1    0x04
#define CAP_HCS2    0x08
#define CAP_HCC1    0x10
#define CAP_DBOFF   0x14
#define CAP_RTSOFF  0x18
/* Operational */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(n) (0x400 + 0x10 * ((n) - 1))
#define CMD_RUN     (1u << 0)
#define CMD_HCRST   (1u << 1)
#define STS_HCH     (1u << 0)
#define STS_CNR     (1u << 11)
/* PORTSC */
#define PS_CCS      (1u << 0)
#define PS_PED      (1u << 1)
#define PS_PR       (1u << 4)
#define PS_PP       (1u << 9)
#define PS_PRC      (1u << 21)
#define PS_CHANGES  (0x7Fu << 17)       /* write-1-to-clear change bits */
/* Interrupter 0, at the runtime base + 0x20 */
#define IR_ERSTSZ   0x28
#define IR_ERSTBA   0x30
#define IR_ERDP     0x38

/* TRB types */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS     11
#define TRB_CONFIG_EP   12
#define TRB_EVALUATE    13
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND  33
#define TRB_CYCLE   (1u << 0)
#define TRB_TC      (1u << 1)           /* link: toggle cycle */
#define TRB_ISP     (1u << 2)
#define TRB_CH      (1u << 4)
#define TRB_IOC     (1u << 5)
#define TRB_IDT     (1u << 6)
#define CC_SUCCESS  1
#define CC_SHORT    13

#define MSG_USB_DP  0x05

struct trb {
    u64 param;
    u32 status;
    u32 control;
};

struct ring {
    struct trb *trb;
    u32 enq;
    u32 cycle;
};

struct xhci {
    uptr cap, op, rt, db;
    u32 ctx_size;                       /* 32 or 64 bytes */
    u32 ports, slots;
    u64 *dcbaa;
    struct ring cmd;
    struct trb *events;
    u32 ev_deq, ev_cycle;
    u8 *bounce;                         /* data */
    u8 *small;                          /* a CBW at 0, a CSW at 64 */
};

struct usb_dev {
    struct sq_blk blk;                  /* first */
    struct xhci *hc;
    u32 slot, port;
    struct ring ep0, in, out;           /* control, bulk IN, bulk OUT */
    u32 dci_in, dci_out;
    u32 mps_in, mps_out;
    u8 *in_ctx, *out_ctx;
    u32 tag;
};

static EFI_STATUS page_alloc(UINTN pages, void **out)
{
    EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;    /* 32-bit DMA always works */
    EFI_STATUS s = efi_allocate_pages(AllocateMaxAddress, EfiBootServicesData, pages, &a);
    if (s == EFI_SUCCESS) {
        *out = (void *)(uptr)a;
        memset(*out, 0, pages * 4096);
    }
    return s;
}

static int ring_init(struct ring *r)
{
    if (page_alloc(1, (void **)&r->trb) != EFI_SUCCESS)
        return 0;
    r->enq = 0;
    r->cycle = 1;
    struct trb *link = &r->trb[RING_TRBS - 1];
    link->param = (uptr)r->trb;
    link->control = TRB_LINK << 10 | TRB_TC;
    return 1;
}

/* Put a TRB on a ring (its cycle bit last); the TRB's address */
static u64 ring_put(struct ring *r, u64 param, u32 status, u32 control)
{
    struct trb *t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    __sync_synchronize();
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    u64 at = (uptr)t;
    if (++r->enq == RING_TRBS - 1) {    /* hand the link over and wrap */
        struct trb *link = &r->trb[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle | (control & TRB_CH);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void ring_db(struct xhci *hc, u32 slot, u32 target)
{
    __sync_synchronize();
    mmio_write32(hc->db + 4 * slot, target);
}

static void w64(uptr a, u64 v)
{
    mmio_write32(a, (u32)v);
    mmio_write32(a + 4, (u32)(v >> 32));
}

/* The next event of type want (others are dropped), or 0 */
static struct trb *event_wait(struct xhci *hc, u32 want)
{
    for (u64 spins = 0; spins < SPINS; spins++) {
        struct trb *e = &hc->events[hc->ev_deq];
        if ((e->control & TRB_CYCLE) != hc->ev_cycle)
            continue;
        __sync_synchronize();
        if (++hc->ev_deq == RING_TRBS) {
            hc->ev_deq = 0;
            hc->ev_cycle ^= 1;
        }
        w64(hc->rt + IR_ERDP, (uptr)&hc->events[hc->ev_deq] | 8);  /* EHB */
        if ((e->control >> 10 & 0x3F) == want)
            return e;
    }
    return NULL;
}

/* A command: its completion code; the slot ID in *slot */
static u32 command(struct xhci *hc, u64 param, u32 control, u32 *slot)
{
    ring_put(&hc->cmd, param, 0, control);
    ring_db(hc, 0, 0);
    struct trb *e = event_wait(hc, TRB_EV_COMMAND);
    if (!e)
        return 0;
    if (slot)
        *slot = e->control >> 24;
    return e->status >> 24;
}

static u8 *ctx(struct usb_dev *d, u32 index)    /* input context entry */
{
    return d->in_ctx + index * d->hc->ctx_size;
}

static void wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

/* Endpoint context dci in the input context: type, max packet, ring */
static void ep_ctx(struct usb_dev *d, u32 dci, u32 type, u32 mps, struct ring *r)
{
    u8 *e = ctx(d, dci + 1);
    memset(e, 0, d->hc->ctx_size);
    wr32(e + 4, 3u << 1 | type << 3 | mps << 16);      /* CErr 3 */
    u64 deq = (uptr)r->trb | 1;
    memcpy(e + 8, &deq, 8);
    wr32(e + 16, type == 4 ? 8 : mps);                 /* average TRB length */
}

/* Wait for the transfer event of a TD on (slot, dci): 1 on success; the
 * bytes not transferred in *residue */
static int transfer_wait(struct usb_dev *d, u32 dci, u32 *residue)
{
    for (;;) {
        struct trb *e = event_wait(d->hc, TRB_EV_TRANSFER);
        if (!e)
            return 0;
        if ((e->control >> 24) != d->slot || (e->control >> 16 & 0x1F) != dci)
            continue;
        u32 cc = e->status >> 24;
        if (residue)
            *residue = e->status & 0xFFFFFF;
        return cc == CC_SUCCESS || cc == CC_SHORT;
    }
}

/* A control transfer on EP0; data through the bounce buffer */
static int control(struct usb_dev *d, u8 type, u8 req, u16 value, u16 index, u16 len)
{
    u64 setup = type | (u32)req << 8 | (u32)value << 16 | (u64)index << 32 | (u64)len << 48;
    int in = (type & 0x80) != 0;
    u32 trt = !len ? 0 : in ? 3 : 2;
    ring_put(&d->ep0, setup, 8, TRB_SETUP << 10 | TRB_IDT | trt << 16);
    if (len)
        ring_put(&d->ep0, (uptr)d->hc->bounce, len, TRB_DATA << 10 | (in ? 1u << 16 : 0));
    ring_put(&d->ep0, 0, 0, TRB_STATUS << 10 | TRB_IOC | ((in && len) ? 0 : 1u << 16));
    ring_db(d->hc, d->slot, 1);
    return transfer_wait(d, 1, NULL);
}

/* A bulk transfer of n bytes at buf (page-aligned memory of ours), split
 * where it crosses 64 KB */
static int bulk(struct usb_dev *d, int in, void *buf, u32 n, u32 *residue)
{
    struct ring *r = in ? &d->in : &d->out;
    u64 a = (uptr)buf, end = a + n;
    do {
        u64 stop = (a | 0xFFFF) + 1;
        if (stop > end)
            stop = end;
        int last = stop == end;
        ring_put(r, a, (u32)(stop - a), TRB_NORMAL << 10 | (last ? TRB_IOC : TRB_CH) | TRB_ISP);
        a = stop;
    } while (a < end);
    ring_db(d->hc, d->slot, in ? d->dci_in : d->dci_out);
    return transfer_wait(d, in ? d->dci_in : d->dci_out, residue);
}

/* Bulk-only transport: one SCSI command, n bytes of data (in or out)
 * through the bounce buffer, its status */
static int scsi(struct usb_dev *d, const u8 *cdb, u8 cdb_len, u32 n, int in)
{
    u8 *cbw = d->hc->small, *csw = d->hc->small + 64;
    memset(cbw, 0, 31);
    wr32(cbw, 0x43425355);                  /* "USBC" */
    wr32(cbw + 4, ++d->tag);
    wr32(cbw + 8, n);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    if (!bulk(d, 0, cbw, 31, NULL))
        return 0;
    if (n && !bulk(d, in, d->hc->bounce, n, NULL))
        return 0;
    memset(csw, 0, 13);
    if (!bulk(d, 1, csw, 13, NULL))
        return 0;
    return rd32(csw) == 0x53425355 && rd32(csw + 4) == d->tag && csw[12] == 0;
}

static int rw(struct usb_dev *d, int write, u64 lba, u8 *buf, u32 bytes)
{
    u8 *b = d->hc->bounce;
    while (bytes) {
        u32 n = bytes > BOUNCE ? BOUNCE : bytes;
        u8 cdb[16];
        memset(cdb, 0, sizeof cdb);
        u8 len = 10;
        if (lba + n / 512 > 0xFFFFFFFFULL) {
            cdb[0] = write ? 0x8A : 0x88;   /* WRITE(16) / READ(16) */
            for (int i = 0; i < 8; i++)
                cdb[2 + i] = (u8)(lba >> (56 - 8 * i));
            u32 blocks = n / 512;
            cdb[10] = (u8)(blocks >> 24);
            cdb[11] = (u8)(blocks >> 16);
            cdb[12] = (u8)(blocks >> 8);
            cdb[13] = (u8)blocks;
            len = 16;
        } else {
            cdb[0] = write ? 0x2A : 0x28;   /* WRITE(10) / READ(10) */
            cdb[2] = (u8)(lba >> 24);
            cdb[3] = (u8)(lba >> 16);
            cdb[4] = (u8)(lba >> 8);
            cdb[5] = (u8)lba;
            cdb[7] = (u8)(n / 512 >> 8);
            cdb[8] = (u8)(n / 512);
        }
        if (write)
            memcpy(b, buf, n);
        if (!scsi(d, cdb, len, n, !write))
            return 0;
        if (!write)
            memcpy(buf, b, n);
        lba += n / 512;
        buf += n;
        bytes -= n;
    }
    return 1;
}

static int usb_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return rw((struct usb_dev *)b, 0, lba, buf, bytes);
}

static int usb_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return rw((struct usb_dev *)b, 1, lba, buf, bytes);
}

/* A bulk-only SCSI interface in the configuration descriptor (in the
 * bounce buffer): its endpoints; the configuration's value */
static int find_bot(struct usb_dev *d, u32 total, u8 *config)
{
    u8 *p = d->hc->bounce;
    *config = p[5];
    int in_iface = 0;
    for (u32 at = 0; at + 2 <= total && p[at] >= 2; at += p[at]) {
        u8 *x = p + at;
        if (x[1] == 4)                      /* interface */
            in_iface = x[5] == 8 && x[6] == 6 && x[7] == 0x50;
        else if (x[1] == 5 && in_iface && (x[3] & 3) == 2) {    /* bulk endpoint */
            u32 num = x[2] & 0x0F, mps = x[4] | (x[5] & 7) << 8;
            if (x[2] & 0x80) {
                d->dci_in = 2 * num + 1;
                d->mps_in = mps;
            } else {
                d->dci_out = 2 * num;
                d->mps_out = mps;
            }
        }
    }
    return d->dci_in && d->dci_out;
}

/* Pci(..)/USB(port - 1, interface 0) */
static EFI_DEVICE_PATH_PROTOCOL *usb_path(EFI_DEVICE_PATH_PROTOCOL *pci, u32 port)
{
    u8 node[10] = {
        MESSAGING_DEVICE_PATH, MSG_USB_DP, 6, 0, (u8)(port - 1), 0,
        END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0
    };
    return efi_dp_append(pci, (const EFI_DEVICE_PATH_PROTOCOL *)node);
}

/* Reset a root port with something on it: its speed, or 0 */
static u32 port_reset(struct xhci *hc, u32 port)
{
    uptr ps = hc->op + OP_PORTSC(port);
    u32 v = mmio_read32(ps);
    if (!(v & PS_CCS))
        return 0;
    if (!(v & PS_PED)) {                    /* USB 3 ports enable themselves */
        mmio_write32(ps, (v & ~(PS_CHANGES | PS_PED)) | PS_PR);
        u64 i = 0;
        while (!(mmio_read32(ps) & PS_PRC) && ++i < SPINS)
            ;
    }
    v = mmio_read32(ps);
    mmio_write32(ps, (v & ~PS_PED) | (v & PS_CHANGES));    /* clear the changes */
    return (v & PS_PED) ? (v >> 10 & 0xF) : 0;
}

static int device_add(struct sq_pci *pci, struct xhci *hc, u32 port, EFI_DEVICE_PATH_PROTOCOL *pp)
{
    u32 speed = port_reset(hc, port);
    if (!speed)
        return 0;
    struct usb_dev *d;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *d, (void **)&d) != EFI_SUCCESS)
        return 0;
    memset(d, 0, sizeof *d);
    d->hc = hc;
    d->port = port;
    if (command(hc, 0, TRB_ENABLE_SLOT << 10, &d->slot) != CC_SUCCESS || !d->slot ||
        d->slot > hc->slots)
        return 0;
    if (page_alloc(1, (void **)&d->in_ctx) != EFI_SUCCESS ||
        page_alloc(1, (void **)&d->out_ctx) != EFI_SUCCESS || !ring_init(&d->ep0) ||
        !ring_init(&d->in) || !ring_init(&d->out))
        return 0;
    hc->dcbaa[d->slot] = (uptr)d->out_ctx;

    /* Address the device: slot and EP0 contexts */
    u32 mps0 = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    wr32(ctx(d, 0) + 4, 3);                 /* add slot and EP0 */
    wr32(ctx(d, 1), speed << 20 | 1u << 27);
    wr32(ctx(d, 1) + 4, port << 16);
    ep_ctx(d, 1, 4, mps0, &d->ep0);
    if (command(hc, (uptr)d->in_ctx, TRB_ADDRESS << 10 | d->slot << 24, NULL) != CC_SUCCESS) {
        printf("usb: port %u: no address\n", port);
        return 0;
    }
    u8 *b = hc->bounce;
    if (!control(d, 0x80, 6, 0x0100, 0, 8))         /* device descriptor, 8 bytes */
        return 0;
    if (b[7] && b[7] != mps0 && speed < 4) {        /* EP0's real packet size */
        mps0 = b[7];
        wr32(ctx(d, 0), 0);
        wr32(ctx(d, 0) + 4, 2);
        ep_ctx(d, 1, 4, mps0, &d->ep0);
        u64 deq = (uptr)&d->ep0.trb[d->ep0.enq] | d->ep0.cycle;
        memcpy(ctx(d, 2) + 8, &deq, 8);
        if (command(hc, (uptr)d->in_ctx, TRB_EVALUATE << 10 | d->slot << 24, NULL) != CC_SUCCESS)
            return 0;
    }
    if (!control(d, 0x80, 6, 0x0100, 0, 18))
        return 0;
    u16 vid = (u16)(b[8] | b[9] << 8), pid = (u16)(b[10] | b[11] << 8);
    if (!control(d, 0x80, 6, 0x0200, 0, 9))         /* configuration, header */
        return 0;
    u32 total = b[2] | b[3] << 8;
    if (total > 1024)
        total = 1024;
    u8 config;
    if (!control(d, 0x80, 6, 0x0200, 0, (u16)total) || !find_bot(d, total, &config)) {
        printf("usb: port %u: %04x:%04x, not a bulk-only disk\n", port, vid, pid);
        return 0;
    }
    if (!control(d, 0x00, 9, config, 0, 0))         /* SET_CONFIGURATION */
        return 0;

    /* The bulk endpoints */
    u32 last = d->dci_in > d->dci_out ? d->dci_in : d->dci_out;
    memset(d->in_ctx, 0, 4096);
    wr32(ctx(d, 0) + 4, 1u | 1u << d->dci_in | 1u << d->dci_out);
    memcpy(ctx(d, 1), d->out_ctx, hc->ctx_size);    /* the slot as it is now */
    wr32(ctx(d, 1), (rd32(ctx(d, 1)) & ~(0x1Fu << 27)) | last << 27);
    wr32(ctx(d, 1) + 12, 0);
    ep_ctx(d, d->dci_out, 2, d->mps_out, &d->out);
    ep_ctx(d, d->dci_in, 6, d->mps_in, &d->in);
    if (command(hc, (uptr)d->in_ctx, TRB_CONFIG_EP << 10 | d->slot << 24, NULL) != CC_SUCCESS) {
        printf("usb: port %u: endpoints not configured\n", port);
        return 0;
    }

    /* READ CAPACITY(10), once more after a unit attention */
    u8 cdb[16];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x25;
    int ok = scsi(d, cdb, 10, 8, 1);
    if (!ok) {
        memset(cdb, 0, sizeof cdb);         /* TEST UNIT READY, then again */
        scsi(d, cdb, 6, 0, 0);
        cdb[0] = 0x25;
        ok = scsi(d, cdb, 10, 8, 1);
    }
    u64 last_lba = ok ? (u32)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) : 0;
    u32 block = ok ? (u32)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]) : 0;
    if (ok && last_lba == 0xFFFFFFFF) {     /* READ CAPACITY(16) */
        memset(cdb, 0, sizeof cdb);
        cdb[0] = 0x9E;
        cdb[1] = 0x10;
        cdb[13] = 32;
        ok = scsi(d, cdb, 16, 32, 1);
        last_lba = 0;
        for (int i = 0; i < 8; i++)
            last_lba = last_lba << 8 | b[i];
        block = (u32)(b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11]);
    }
    if (!ok || block != 512) {
        printf("usb: port %u: %04x:%04x, no 512-byte disk\n", port, vid, pid);
        return 0;
    }
    d->blk.sectors = last_lba + 1;
    d->blk.path = usb_path(pp, port);
    d->blk.read = usb_read;
    d->blk.write = usb_write;
    printf("disk: USB at PCI %02x:%02x.%x port %u (%04x:%04x, bulk-only), %lu sectors\n",
           pci->bus, pci->dev, pci->fn, port, vid, pid, d->blk.sectors);
    efi_disk_add(&d->blk);
    return 1;
}

int xhci_add(struct sq_pci *pci)
{
    uptr base = (uptr)pci->bar[0];
    if (!base)
        return 0;
    struct xhci *hc;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *hc, (void **)&hc) != EFI_SUCCESS)
        return 0;
    memset(hc, 0, sizeof *hc);
    hc->cap = base;
    hc->op = base + mmio_read8(base + CAP_LENGTH);
    hc->rt = base + (mmio_read32(base + CAP_RTSOFF) & ~0x1Fu);
    hc->db = base + (mmio_read32(base + CAP_DBOFF) & ~0x3u);
    u32 hcs1 = mmio_read32(base + CAP_HCS1), hcs2 = mmio_read32(base + CAP_HCS2);
    hc->ports = hcs1 >> 24;
    hc->slots = (hcs1 & 0xFF) < MAX_SLOTS ? hcs1 & 0xFF : MAX_SLOTS;
    hc->ctx_size = (mmio_read32(base + CAP_HCC1) & 4) ? 64 : 32;

    /* Stop and reset it */
    mmio_write32(hc->op + OP_USBCMD, mmio_read32(hc->op + OP_USBCMD) & ~CMD_RUN);
    for (u64 i = 0; !(mmio_read32(hc->op + OP_USBSTS) & STS_HCH) && i < SPINS; i++)
        ;
    mmio_write32(hc->op + OP_USBCMD, CMD_HCRST);
    for (u64 i = 0; (mmio_read32(hc->op + OP_USBCMD) & CMD_HCRST) && i < SPINS; i++)
        ;
    for (u64 i = 0; (mmio_read32(hc->op + OP_USBSTS) & STS_CNR) && i < SPINS; i++)
        ;
    if (mmio_read32(hc->op + OP_USBSTS) & STS_CNR)
        return 0;

    /* Device contexts (and scratchpads), the command and event rings */
    u32 scratch = (hcs2 >> 27 & 0x1F) | (hcs2 >> 21 & 0x1F) << 5;
    if (page_alloc(1, (void **)&hc->dcbaa) != EFI_SUCCESS || !ring_init(&hc->cmd) ||
        page_alloc(1, (void **)&hc->events) != EFI_SUCCESS ||
        page_alloc(BOUNCE / 4096, (void **)&hc->bounce) != EFI_SUCCESS ||
        page_alloc(1, (void **)&hc->small) != EFI_SUCCESS)
        return 0;
    if (scratch) {
        u64 *arr;
        void *pages;
        if (page_alloc(1, (void **)&arr) != EFI_SUCCESS || page_alloc(scratch, &pages) != EFI_SUCCESS)
            return 0;
        for (u32 i = 0; i < scratch && i < 512; i++)
            arr[i] = (uptr)pages + 4096ULL * i;
        hc->dcbaa[0] = (uptr)arr;
    }
    u64 *erst;
    if (page_alloc(1, (void **)&erst) != EFI_SUCCESS)
        return 0;
    erst[0] = (uptr)hc->events;
    erst[1] = RING_TRBS;
    hc->ev_deq = 0;
    hc->ev_cycle = 1;

    mmio_write32(hc->op + OP_CONFIG, hc->slots);
    w64(hc->op + OP_DCBAAP, (uptr)hc->dcbaa);
    w64(hc->op + OP_CRCR, (uptr)hc->cmd.trb | 1);
    mmio_write32(hc->rt + IR_ERSTSZ, 1);
    w64(hc->rt + IR_ERDP, (uptr)hc->events);
    w64(hc->rt + IR_ERSTBA, (uptr)erst);
    mmio_write32(hc->op + OP_USBCMD, CMD_RUN);
    for (u64 i = 0; (mmio_read32(hc->op + OP_USBSTS) & STS_HCH) && i < SPINS; i++)
        ;

    /* Ports get power on their own here (no PPC) or now */
    for (u32 p = 1; p <= hc->ports; p++) {
        uptr ps = hc->op + OP_PORTSC(p);
        u32 v = mmio_read32(ps);
        if (!(v & PS_PP))
            mmio_write32(ps, (v & ~(PS_CHANGES | PS_PED)) | PS_PP);
    }
    int n = 0;
    for (u32 p = 1; p <= hc->ports; p++)
        n += device_add(pci, hc, p, pci->path);
    return n;
}
