/* SquirrelBIOS UEFI core (common/efi): NVMe disks. Original code, from
 * the NVM Express Base specification 1.4 (controller registers, admin
 * Identify / Create I/O Queue commands, PRPs) and the NVM Command Set
 * (Read, Write).
 *
 * One admin queue pair and one I/O queue pair of QSIZE entries, polled
 * (no interrupts); one command at a time. Namespace 1 is the disk and
 * must use 512-byte blocks. Data goes through a page-aligned bounce
 * buffer of BOUNCE bytes described by a PRP list. */
#include "efi.h"

#define REG_CAP     0x00
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28
#define REG_ACQ     0x30
#define REG_DB      0x1000

#define CC_EN       1u
#define CC_IOSQES   (6u << 16)      /* 64-byte submission entries */
#define CC_IOCQES   (4u << 20)      /* 16-byte completion entries */
#define CSTS_RDY    1u
#define CSTS_CFS    2u

#define OP_CREATE_SQ  0x01
#define OP_CREATE_CQ  0x05
#define OP_IDENTIFY   0x06
#define OP_WRITE      0x01
#define OP_READ       0x02

#define QSIZE       16
#define PAGE        4096u
#define BOUNCE      (128u * 1024)
#define SPINS       200000000ULL

/* Hardware namespace node: messaging, NVM Express namespace */
#define MSG_NVME_NAMESPACE_DP 0x17

struct queue {
    u8 *sq, *cq;
    u16 tail, head, cid;
    u8 phase;
    uptr sq_db, cq_db;
};

struct nvme {
    struct sq_blk blk;          /* first */
    uptr regs;
    struct queue admin, io;
    u8 *page;                   /* Identify data, then the PRP list */
    u8 *bounce;
    u32 max_bytes;              /* per command */
};

static int wait_ready(uptr regs, u32 want)
{
    for (u64 i = 0; i < SPINS; i++) {
        u32 s = mmio_read32(regs + REG_CSTS);
        if (s & CSTS_CFS)
            return 0;
        if ((s & CSTS_RDY) == want)
            return 1;
    }
    return 0;
}

/* Submit one 64-byte command, wait for its completion: 1 on success */
static int submit(struct queue *q, u32 *cmd)
{
    cmd[0] = (cmd[0] & 0xFFFF) | (u32)q->cid++ << 16;
    memcpy(q->sq + 64 * q->tail, cmd, 64);
    q->tail = (q->tail + 1) % QSIZE;
    __sync_synchronize();
    mmio_write32(q->sq_db, q->tail);

    volatile u32 *e = (volatile u32 *)(q->cq + 16 * q->head);
    for (u64 i = 0; ((e[3] >> 16) & 1) != q->phase; i++) {
        if (i > SPINS)
            return 0;
        __sync_synchronize();
    }
    u32 status = (e[3] >> 17) & 0x7FFF;
    q->head = (q->head + 1) % QSIZE;
    if (!q->head)
        q->phase ^= 1;
    mmio_write32(q->cq_db, q->head);
    return status == 0;
}

static void queue_init(struct nvme *n, struct queue *q, u8 *mem, int id, u32 stride)
{
    q->sq = mem;
    q->cq = mem + PAGE;
    q->tail = q->head = q->cid = 0;
    q->phase = 1;
    q->sq_db = n->regs + REG_DB + (2 * id) * stride;
    q->cq_db = n->regs + REG_DB + (2 * id + 1) * stride;
}

static void cmd_init(u32 *c, u8 op, u32 nsid, void *prp1)
{
    memset(c, 0, 64);
    c[0] = op;
    c[1] = nsid;
    u64 a = (uptr)prp1;
    memcpy(c + 6, &a, 8);
}

static int nvme_rw(struct nvme *n, u8 op, u64 lba, u8 *buf, u32 bytes)
{
    while (bytes) {
        u32 len = bytes > n->max_bytes ? n->max_bytes : bytes;
        u32 c[16];
        cmd_init(c, op, 1, n->bounce);
        u32 pages = (len + PAGE - 1) / PAGE;
        u64 prp2 = 0;
        if (pages == 2)
            prp2 = (uptr)n->bounce + PAGE;
        else if (pages > 2) {
            u64 *list = (u64 *)n->page;
            for (u32 i = 1; i < pages; i++)
                list[i - 1] = (uptr)n->bounce + (u64)i * PAGE;
            prp2 = (uptr)list;
        }
        memcpy(c + 8, &prp2, 8);
        c[10] = (u32)lba;
        c[11] = (u32)(lba >> 32);
        c[12] = len / 512 - 1;
        if (op == OP_WRITE)
            memcpy(n->bounce, buf, len);
        if (!submit(&n->io, c))
            return 0;
        if (op == OP_READ)
            memcpy(buf, n->bounce, len);
        lba += len / 512;
        buf += len;
        bytes -= len;
    }
    return 1;
}

static int nvme_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return nvme_rw((struct nvme *)b, OP_READ, lba, buf, bytes);
}

static int nvme_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return nvme_rw((struct nvme *)b, OP_WRITE, lba, buf, bytes);
}

/* Pci(..)/NVMe(namespace 1, EUI-64 0) */
static EFI_DEVICE_PATH_PROTOCOL *ns_path(EFI_DEVICE_PATH_PROTOCOL *pci)
{
    static const u8 node[20] = {
        MESSAGING_DEVICE_PATH, MSG_NVME_NAMESPACE_DP, 16, 0, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0
    };
    return efi_dp_append(pci, (const EFI_DEVICE_PATH_PROTOCOL *)node);
}

int nvme_add(struct sq_pci *p)
{
    uptr regs = (uptr)p->bar[0];
    if (!regs)
        return 0;
    u64 cap = mmio_read64(regs + REG_CAP);
    u32 stride = 4u << ((cap >> 32) & 0xF);
    if (((cap >> 48) & 0xF) != 0 || (cap & 0xFFFF) + 1 < QSIZE)
        return 0;                   /* needs 4 KB pages, QSIZE entries */

    /* Admin and I/O queue pairs, Identify / PRP list page, bounce buffer */
    u32 npages = 4 + 1 + BOUNCE / PAGE;
    EFI_PHYSICAL_ADDRESS a = 0;
    struct nvme *n;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *n, (void **)&n) != EFI_SUCCESS)
        return 0;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, npages, &a) != EFI_SUCCESS)
        return 0;
    u8 *mem = (u8 *)(uptr)a;
    memset(mem, 0, 5 * PAGE);
    memset(n, 0, sizeof *n);
    n->regs = regs;
    n->page = mem + 4 * PAGE;
    n->bounce = mem + 5 * PAGE;
    queue_init(n, &n->admin, mem, 0, stride);
    queue_init(n, &n->io, mem + 2 * PAGE, 1, stride);

    mmio_write32(regs + REG_CC, 0);
    if (!wait_ready(regs, 0))
        return 0;
    mmio_write32(regs + REG_AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
    mmio_write64(regs + REG_ASQ, (uptr)n->admin.sq);
    mmio_write64(regs + REG_ACQ, (uptr)n->admin.cq);
    mmio_write32(regs + REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (!wait_ready(regs, 1)) {
        printf("disk: NVMe at PCI %02x:%02x.%x did not start\n", p->bus, p->dev, p->fn);
        return 0;
    }

    u32 c[16];
    cmd_init(c, OP_IDENTIFY, 0, n->page);
    c[10] = 1;                      /* controller */
    if (!submit(&n->admin, c))
        return 0;
    u8 mdts = n->page[77];
    n->max_bytes = BOUNCE;
    if (mdts && mdts < 32 && (PAGE << mdts) < BOUNCE)
        n->max_bytes = PAGE << mdts;

    cmd_init(c, OP_IDENTIFY, 1, n->page);
    c[10] = 0;                      /* namespace */
    if (!submit(&n->admin, c))
        return 0;
    u64 nsze;
    memcpy(&nsze, n->page, 8);
    u8 fmt = n->page[26] & 0xF;
    u8 lbads = n->page[128 + 4 * fmt + 2];
    if (!nsze || lbads != 9) {
        printf("disk: NVMe at PCI %02x:%02x.%x: namespace 1 unusable\n", p->bus, p->dev, p->fn);
        return 0;
    }

    cmd_init(c, OP_CREATE_CQ, 0, n->io.cq);
    c[10] = (QSIZE - 1) << 16 | 1;
    c[11] = 1;                      /* physically contiguous, no interrupts */
    if (!submit(&n->admin, c))
        return 0;
    cmd_init(c, OP_CREATE_SQ, 0, n->io.sq);
    c[10] = (QSIZE - 1) << 16 | 1;
    c[11] = 1u << 16 | 1;           /* completion queue 1, contiguous */
    if (!submit(&n->admin, c))
        return 0;

    n->blk.sectors = nsze;
    n->blk.path = ns_path(p->path);
    n->blk.read = nvme_read;
    n->blk.write = nvme_write;
    printf("disk: NVMe at PCI %02x:%02x.%x, %lu sectors\n", p->bus, p->dev, p->fn, nsze);
    efi_disk_add(&n->blk);
    return 1;
}
