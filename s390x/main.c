/* SquirrelBIOS s390x (QEMU s390-ccw-virtio): SCLP console, a virtio-blk
 * IPL over the channel subsystem, and the sq> prompt.
 *
 * Original code, written from the z/Architecture Principles of
 * Operation, the virtio 1.x specification (its virtio over channel I/O
 * transport, legacy interface) and the public SCLP event interface
 * descriptions; not from another firmware's source.
 */
#include "fwlib.h"

#define EM_S390 22

/* ------------------------------------------------------------------ */
/* start.S                                                              */
/* ------------------------------------------------------------------ */
void sclp_wait(void);
void fw_enter(uint64_t entry, uint64_t r2, uint64_t r3) __attribute__((noreturn));
void fw_halt(uint64_t addr) __attribute__((noreturn));
extern char _start[], _end[];

/* ------------------------------------------------------------------ */
/* SCLP                                                                 */
/* ------------------------------------------------------------------ */
#define SCLP_READ_SCP_INFO      0x00020001u
#define SCLP_READ_SCP_INFO_F    0x00120001u
#define SCLP_WRITE_EVENT_MASK   0x00780005u
#define SCLP_WRITE_EVENT_DATA   0x00760005u
#define SCLP_READ_EVENT_DATA    0x00770005u
#define SCLP_RC_OK              0x0020
#define SCLP_RC_OK_INFO         0x0010
#define EV_VT220                0x1a
#define EV_MASK_VT220           0x00000040u     /* bit 25 for type 26 */

struct sccb_hdr {
    uint16_t length;
    uint8_t fc;
    uint8_t control[3];
    uint16_t rc;
} __attribute__((packed));

struct ev_hdr {
    uint16_t length;
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
} __attribute__((packed));

static uint8_t sccb[4096] __attribute__((aligned(4096)));
static int sclp_ok;
static unsigned sclp_errors;

static int servc(uint32_t cmd, void *buf)
{
    int cc;
    uint64_t addr = (uint64_t)buf;
    asm volatile(".insn rre,0xb2200000,%1,%2\n\tipm %0\n\tsrl %0,28"
                 : "=d"(cc) : "d"(cmd), "a"(addr) : "cc", "memory");
    if (cc == 0)
        sclp_wait();
    return cc;
}

static int sclp_cmd(uint32_t cmd)
{
    struct sccb_hdr *h = (struct sccb_hdr *)sccb;
    if (servc(cmd, sccb) != 0) {
        sclp_errors++;
        return -1;
    }
    return h->rc;
}

static void sclp_set_mask(uint32_t receive, uint32_t send)
{
    struct sccb_hdr *h = (struct sccb_hdr *)sccb;
    memset(sccb, 0, 64);
    h->length = 8 + 4 + 4 * 4;
    *(uint16_t *)(sccb + 10) = 4;               /* mask length */
    *(uint32_t *)(sccb + 12) = receive;         /* what we take in */
    *(uint32_t *)(sccb + 16) = send;            /* what we send */
    sclp_ok = sclp_cmd(SCLP_WRITE_EVENT_MASK) == SCLP_RC_OK;
}

/* Output: lines are buffered and sent as one VT220 event each */
static char out_buf[256];
static int out_len;

static void sclp_flush(void)
{
    struct sccb_hdr *h = (struct sccb_hdr *)sccb;
    struct ev_hdr *e = (struct ev_hdr *)(sccb + 8);
    if (!out_len || !sclp_ok) {
        out_len = 0;
        return;
    }
    memset(sccb, 0, 16);
    h->length = 8 + 6 + out_len;
    e->length = 6 + out_len;
    e->type = EV_VT220;
    memcpy(sccb + 14, out_buf, out_len);
    out_len = 0;
    sclp_cmd(SCLP_WRITE_EVENT_DATA);
}

void fw_putc(char c)
{
    /* QEMU's VT220 side already turns \n into \r\n */
    out_buf[out_len++] = c;
    if (c == '\n' || out_len >= (int)sizeof(out_buf) - 2)
        sclp_flush();
}

/* Input: one read event fills in_buf, fw_getc hands it out */
static char in_buf[128];
static int in_len, in_pos;

int fw_getc(void)
{
    struct sccb_hdr *h = (struct sccb_hdr *)sccb;
    sclp_flush();           /* a prompt without a newline */
    if (in_pos < in_len)
        return (uint8_t)in_buf[in_pos++];
    in_len = in_pos = 0;
    if (!sclp_ok)
        return -1;
    memset(sccb, 0, 16);
    h->length = sizeof(sccb);
    if (sclp_cmd(SCLP_READ_EVENT_DATA) != SCLP_RC_OK)
        return -1;
    for (unsigned off = 8; off + 6 <= h->length && off < sizeof(sccb);) {
        struct ev_hdr *e = (struct ev_hdr *)(sccb + off);
        if (e->length < 6)
            break;
        if (e->type == EV_VT220) {
            for (unsigned i = 6; i < e->length && in_len < (int)sizeof(in_buf); i++)
                in_buf[in_len++] = sccb[off + i];
        }
        off += e->length;
    }
    if (in_pos < in_len)
        return (uint8_t)in_buf[in_pos++];
    return -1;
}

static uint64_t sclp_ram_size(void)
{
    struct sccb_hdr *h = (struct sccb_hdr *)sccb;
    uint32_t cmds[2] = { SCLP_READ_SCP_INFO_F, SCLP_READ_SCP_INFO };
    for (int i = 0; i < 2; i++) {
        memset(sccb, 0, sizeof(sccb));
        h->length = sizeof(sccb);
        int rc = sclp_cmd(cmds[i]);
        if (rc != SCLP_RC_OK && rc != SCLP_RC_OK_INFO)
            continue;
        uint64_t rnmax = *(uint16_t *)(sccb + 8);
        uint64_t rnsize = sccb[10];
        if (!rnsize)
            rnsize = *(uint32_t *)(sccb + 100);
        if (!rnmax)
            rnmax = *(uint64_t *)(sccb + 104);
        return rnmax * rnsize << 20;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Channel subsystem                                                    */
/* ------------------------------------------------------------------ */
struct pmcw {
    uint32_t intparm;
    uint32_t flags;             /* ISC, E (0x00800000), V (0x00010000), devno */
    uint32_t paths;
    uint32_t mbi;
    uint8_t chpid[8];
    uint32_t misc;
} __attribute__((packed));

struct scsw {
    uint32_t flags;
    uint32_t ccw;
    uint8_t dstat;
    uint8_t cstat;
    uint16_t count;
} __attribute__((packed));

struct schib {
    struct pmcw pmcw;
    struct scsw scsw;
    uint8_t mda[12];
} __attribute__((packed, aligned(4)));

struct irb {
    struct scsw scsw;
    uint8_t rest[84];
} __attribute__((packed, aligned(4)));

struct orb {
    uint32_t intparm;
    uint32_t flags;
    uint32_t cpa;
    uint32_t reserved[5];
} __attribute__((packed, aligned(4)));

struct ccw1 {
    uint8_t cmd;
    uint8_t flags;
    uint16_t count;
    uint32_t cda;
} __attribute__((packed, aligned(8)));

#define PMCW_E          0x00800000u
#define PMCW_V          0x00010000u
#define CCW_CC          0x40
#define CCW_SLI         0x20
#define DEV_CE          0x08
#define DEV_DE          0x04
#define DEV_UC          0x02

static int cc_of(int ipm) { return (ipm >> 28) & 3; }

static int stsch(uint32_t schid, struct schib *s)
{
    register uint32_t r1 asm("1") = schid;
    int ipm;
    asm volatile("stsch 0(%2)\n\tipm %0" : "=d"(ipm) : "d"(r1), "a"(s) : "cc", "memory");
    return cc_of(ipm);
}

static int msch(uint32_t schid, struct schib *s)
{
    register uint32_t r1 asm("1") = schid;
    int ipm;
    asm volatile("msch 0(%2)\n\tipm %0" : "=d"(ipm) : "d"(r1), "a"(s) : "cc", "memory");
    return cc_of(ipm);
}

static int ssch(uint32_t schid, struct orb *o)
{
    register uint32_t r1 asm("1") = schid;
    int ipm;
    asm volatile("ssch 0(%2)\n\tipm %0" : "=d"(ipm) : "d"(r1), "a"(o) : "cc", "memory");
    return cc_of(ipm);
}

static int tsch(uint32_t schid, struct irb *i)
{
    register uint32_t r1 asm("1") = schid;
    int ipm;
    asm volatile("tsch 0(%2)\n\tipm %0" : "=d"(ipm) : "d"(r1), "a"(i) : "cc", "memory");
    return cc_of(ipm);
}

/* Runs one CCW (chained ones follow in the array) and waits for the end
 * of the channel program. Returns 0, or -1 on an error or unit check. */
static int run_ccw(uint32_t schid, struct ccw1 *ccw)
{
    static struct orb orb;
    static struct irb irb;
    memset(&orb, 0, sizeof(orb));
    orb.intparm = schid;
    orb.flags = 0x0080ff00;             /* format-1 CCWs, all paths */
    orb.cpa = (uint32_t)(uint64_t)ccw;
    if (ssch(schid, &orb) != 0)
        return -1;
    for (long spins = 0; spins < 50000000; spins++) {
        if (tsch(schid, &irb) != 0)
            continue;                   /* nothing pending yet */
        if (irb.scsw.cstat || (irb.scsw.dstat & DEV_UC))
            return -1;
        if (irb.scsw.dstat & DEV_DE)
            return 0;
    }
    return -1;
}

static int ccw_do(uint32_t schid, uint8_t cmd, void *data, uint16_t len)
{
    static struct ccw1 ccw;
    ccw.cmd = cmd;
    ccw.flags = CCW_SLI;
    ccw.count = len;
    ccw.cda = (uint32_t)(uint64_t)data;
    return run_ccw(schid, &ccw);
}

/* ------------------------------------------------------------------ */
/* virtio-ccw (legacy interface, revision 0), virtio-blk                */
/* ------------------------------------------------------------------ */
#define CCW_SENSE_ID        0xe4
#define VCCW_WRITE_FEAT     0x11
#define VCCW_SET_VQ         0x13
#define VCCW_READ_CONF      0x22
#define VCCW_WRITE_STATUS   0x31
#define VCCW_READ_VQ_CONF   0x32
#define VCCW_RESET          0x33
#define VIRTIO_CU_TYPE      0x3832
#define VIRTIO_ID_NET       1
#define VIRTIO_ID_BLOCK     2
#define VIRTIO_ID_CONSOLE   3

#define VQ_MAX      256
#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

struct vdesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct dev {
    uint32_t schid;
    uint16_t devno;
    uint16_t cu_type;
    uint8_t cu_model;
};

#define MAX_DEVS 16
static struct dev devs[MAX_DEVS];
static int ndevs;

static uint8_t sense_buf[32] __attribute__((aligned(8)));

static void css_scan(void)
{
    static struct schib s;
    ndevs = 0;
    for (uint32_t n = 0; n < 0x10000 && ndevs < MAX_DEVS; n++) {
        uint32_t schid = 0x00010000u | n;
        int cc = stsch(schid, &s);
        if (cc == 3)
            break;                      /* past the last subchannel */
        if (cc != 0 || !(s.pmcw.flags & PMCW_V))
            continue;
        if (!(s.pmcw.flags & PMCW_E)) {
            s.pmcw.flags |= PMCW_E;
            s.pmcw.intparm = schid;
            if (msch(schid, &s) != 0)
                continue;
        }
        struct dev *d = &devs[ndevs];
        d->schid = schid;
        d->devno = s.pmcw.flags & 0xffff;
        memset(sense_buf, 0, sizeof(sense_buf));
        if (ccw_do(schid, CCW_SENSE_ID, sense_buf, 7) == 0) {
            d->cu_type = (uint16_t)(sense_buf[1] << 8 | sense_buf[2]);
            d->cu_model = sense_buf[3];
        }
        ndevs++;
    }
}

static const char *dev_kind(const struct dev *d)
{
    if (d->cu_type != VIRTIO_CU_TYPE)
        return "unknown";
    switch (d->cu_model) {
    case VIRTIO_ID_NET: return "virtio-net";
    case VIRTIO_ID_BLOCK: return "virtio-blk";
    case VIRTIO_ID_CONSOLE: return "virtio-console";
    default: return "virtio";
    }
}

/* One request queue: descriptors, available ring, then the used ring on
 * the next 4 KB boundary, as the legacy layout wants */
static uint8_t vring_mem[4 * 4096] __attribute__((aligned(4096)));
static struct {
    uint32_t schid;
    uint16_t num;
    uint16_t avail_idx, used_idx;
    struct vdesc *desc;
    uint16_t *avail;            /* flags, idx, ring[num] */
    volatile uint16_t *used;    /* flags, idx, then {u32 id, u32 len}[num] */
    uint64_t sectors;
} blk;

static struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
} __attribute__((packed, aligned(8))) blk_req;
static volatile uint8_t blk_status;

static int blk_open(const struct dev *d)
{
    static uint8_t status;
    static struct { uint32_t features; uint8_t index; } __attribute__((packed, aligned(8))) feat;
    static struct { uint16_t index; uint16_t num; } __attribute__((aligned(8))) vqc;
    static struct {
        uint64_t queue;
        uint32_t align;
        uint16_t index;
        uint16_t num;
    } __attribute__((packed, aligned(8))) vq;
    static uint64_t config[2] __attribute__((aligned(8)));
    uint32_t s = d->schid;

    if (ccw_do(s, VCCW_RESET, 0, 0))
        return -1;
    status = 1 | 2;                     /* ACKNOWLEDGE, DRIVER */
    if (ccw_do(s, VCCW_WRITE_STATUS, &status, 1))
        return -1;
    feat.features = 0;                  /* no optional features */
    feat.index = 0;
    if (ccw_do(s, VCCW_WRITE_FEAT, &feat, sizeof(feat)))
        return -1;
    vqc.index = 0;
    vqc.num = 0;
    if (ccw_do(s, VCCW_READ_VQ_CONF, &vqc, sizeof(vqc)) || !vqc.num)
        return -1;
    blk.num = vqc.num > VQ_MAX ? VQ_MAX : vqc.num;
    if (blk.num != vqc.num)
        return -1;                      /* the device decides the size here */
    memset(vring_mem, 0, sizeof(vring_mem));
    blk.desc = (struct vdesc *)vring_mem;
    blk.avail = (uint16_t *)(vring_mem + 16 * blk.num);
    uint64_t used_off = (16 * blk.num + 6 + 2 * blk.num + 4095) & ~4095ul;
    blk.used = (volatile uint16_t *)(vring_mem + used_off);
    blk.avail_idx = blk.used_idx = 0;
    vq.queue = (uint64_t)vring_mem;
    vq.align = 4096;
    vq.index = 0;
    vq.num = blk.num;
    if (ccw_do(s, VCCW_SET_VQ, &vq, sizeof(vq)))
        return -1;
    status = 1 | 2 | 4;                 /* DRIVER_OK */
    if (ccw_do(s, VCCW_WRITE_STATUS, &status, 1))
        return -1;
    if (ccw_do(s, VCCW_READ_CONF, config, 8))
        return -1;
    blk.sectors = config[0];
    blk.schid = s;
    return 0;
}

static void vq_notify(uint32_t schid, uint64_t queue)
{
    register uint64_t r1 asm("1") = 3;          /* virtio-ccw notify */
    register uint64_t r2 asm("2") = schid;
    register uint64_t r3 asm("3") = queue;
    asm volatile("diag 2,4,0x500" : "+d"(r2) : "d"(r1), "d"(r3) : "memory", "cc");
}

/* Reads 'count' 512-byte sectors from 'sector' to 'buf' (below 2 GB) */
static int blk_read(uint64_t sector, void *buf, uint32_t count)
{
    uint16_t head = 0;
    blk_req.type = 0;                   /* VIRTIO_BLK_T_IN */
    blk_req.ioprio = 0;
    blk_req.sector = sector;
    blk_status = 0xff;
    blk.desc[0] = (struct vdesc){ (uint64_t)&blk_req, sizeof(blk_req), VRING_DESC_F_NEXT, 1 };
    blk.desc[1] = (struct vdesc){ (uint64_t)buf, count * 512,
                                  VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 2 };
    blk.desc[2] = (struct vdesc){ (uint64_t)&blk_status, 1, VRING_DESC_F_WRITE, 0 };
    blk.avail[2 + blk.avail_idx % blk.num] = head;
    asm volatile("bcr 14,0" ::: "memory");      /* serialize */
    blk.avail[1] = ++blk.avail_idx;
    asm volatile("bcr 14,0" ::: "memory");
    vq_notify(blk.schid, 0);
    for (long spins = 0; spins < 100000000; spins++) {
        if (blk.used[1] != blk.used_idx) {
            blk.used_idx++;
            return blk_status == 0 ? 0 : -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* State, IPL                                                           */
/* ------------------------------------------------------------------ */
static uint64_t ram_size, fw_base;
static const struct dev *boot_dev;

#define LC_SUBCHANNEL_ID 0xb8

static void halt_quiet(void) __attribute__((noreturn));
static void halt_quiet(void)
{
    sclp_flush();
    fw_halt(0xfff);                     /* QEMU: a guest shutdown */
}

static void ipl_from(const struct dev *d)
{
    static uint8_t hdr[4096] __attribute__((aligned(8)));
    struct elf_image img;
    fw_printf("boot: virtio-blk %x.%x.%04x, %lu sectors\n", 0, 0, d->devno,
              (unsigned long)blk.sectors);
    if (!blk.sectors) {
        fw_printf("boot: empty disk\n");
        return;
    }
    memset(hdr, 0, sizeof(hdr));
    if (blk_read(0, hdr, blk.sectors < 8 ? (uint32_t)blk.sectors : 8) != 0) {
        fw_printf("boot: read error\n");
        return;
    }
    int err = elf_probe(hdr, 0, EM_S390, &img);
    if (err == -1 || err == -2 || err == -3) {
        fw_printf("boot: not an s390x ELF image (%s)\n", elf_error(err));
        return;
    }
    /* How much of the file the segments need */
    uint64_t phoff = fw_get_be64(hdr + 32);
    unsigned phentsize = hdr[54] << 8 | hdr[55];
    unsigned phnum = hdr[56] << 8 | hdr[57];
    uint64_t file_end = 0;
    if (phoff + (uint64_t)phentsize * phnum > sizeof(hdr)) {
        fw_printf("boot: program headers past the first 4 KB\n");
        return;
    }
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = hdr + phoff + i * phentsize;
        if (fw_get_be32(ph) != 1)       /* PT_LOAD */
            continue;
        uint64_t end = fw_get_be64(ph + 8) + fw_get_be64(ph + 32);
        if (end > file_end)
            file_end = end;
    }
    uint64_t sectors = (file_end + 511) / 512;
    if (sectors > blk.sectors) {
        fw_printf("boot: image needs %lu sectors, the disk has %lu\n",
                  (unsigned long)sectors, (unsigned long)blk.sectors);
        return;
    }
    /* Stage the file just below the firmware, above where it goes */
    uint64_t stage = (fw_base - sectors * 512) & ~0xffful;
    if (stage <= img.high || stage < 0x10000) {
        fw_printf("boot: image at 0x%lx-0x%lx leaves no room to stage it\n",
                  (unsigned long)img.low, (unsigned long)img.high);
        return;
    }
    for (uint64_t s = 0; s < sectors;) {
        uint32_t n = sectors - s > 128 ? 128 : (uint32_t)(sectors - s);
        if (blk_read(s, (void *)(stage + s * 512), n) != 0) {
            fw_printf("boot: read error at sector %lu\n", (unsigned long)s);
            return;
        }
        s += n;
    }
    err = elf_probe((void *)stage, sectors * 512, EM_S390, &img);
    if (err) {
        fw_printf("boot: bad ELF image (%s)\n", elf_error(err));
        return;
    }
    if (img.low < 0x10000) {
        fw_printf("boot: image at 0x%lx overlaps the lowcore\n", (unsigned long)img.low);
        return;
    }
    if (img.high >= fw_base) {
        fw_printf("boot: image at 0x%lx-0x%lx overlaps the firmware\n",
                  (unsigned long)img.low, (unsigned long)img.high);
        return;
    }
    elf_place((void *)stage, &img);
    fw_printf("boot: ELF at 0x%lx-0x%lx, entry 0x%lx\n", (unsigned long)img.low,
              (unsigned long)img.high, (unsigned long)img.entry_phys);
    fw_printf("jumping to the OS\n");
    sclp_flush();
    asm volatile("st %0,%1" :: "d"(d->schid), "i"(LC_SUBCHANNEL_ID) : "memory");
    fw_enter(img.entry_phys, ram_size, 0);
}

static void try_boot(void)
{
    for (int i = 0; i < ndevs; i++) {
        const struct dev *d = &devs[i];
        if (d->cu_type != VIRTIO_CU_TYPE || d->cu_model != VIRTIO_ID_BLOCK)
            continue;
        if (blk_open(d) != 0) {
            fw_printf("boot: virtio-blk %04x does not start\n", d->devno);
            continue;
        }
        boot_dev = d;
        ipl_from(d);
        return;
    }
    fw_printf("boot: no disk (add -device virtio-blk-ccw)\n");
}

void fw_trap(uint64_t code, uint64_t addr, uint64_t mask)
{
    fw_printf("trap: program check 0x%lx at 0x%lx (psw mask 0x%lx)\n",
              (unsigned long)code, (unsigned long)addr, (unsigned long)mask);
    halt_quiet();
}

/* ------------------------------------------------------------------ */
/* Prompt                                                               */
/* ------------------------------------------------------------------ */
static uint16_t cpu_address(void)
{
    uint16_t a;
    asm volatile("stap %0" : "=Q"(a));
    return a;
}

static void cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    fw_printf("machine: s390-ccw-virtio, cpu address %u\n", cpu_address());
    fw_printf("mem: %lu MB\n", (unsigned long)(ram_size >> 20));
    fw_printf("firmware: 0x%lx-0x%lx\n", (unsigned long)fw_base,
              (unsigned long)(uint64_t)_end);
    fw_printf("console: SCLP VT220, %u failed requests\n", sclp_errors);
    fw_printf("subchannels: %d\n", ndevs);
}

static void cmd_devices(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < ndevs; i++)
        fw_printf("  0.0.%04x  schid 0x%08x  cu %04x/%02x  %s\n", devs[i].devno,
                  devs[i].schid, devs[i].cu_type, devs[i].cu_model, dev_kind(&devs[i]));
    if (!ndevs)
        fw_printf("  no subchannels\n");
}

static void cmd_boot(int argc, char **argv)
{
    (void)argc; (void)argv;
    try_boot();
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    fw_printf("powering off\n");
    halt_quiet();
}

static const struct fw_cmd cmds[] = {
    { "info", "machine, memory and firmware", cmd_info },
    { "devices", "the channel subsystem's devices", cmd_devices },
    { "boot", "IPL from the first virtio-blk disk", cmd_boot },
    { "poweroff", "stop the machine", cmd_poweroff },
    { 0, 0, 0 },
};

void fw_main(void)
{
    fw_base = (uint64_t)_start;
    sclp_set_mask(EV_MASK_VT220, EV_MASK_VT220);
    ram_size = sclp_ram_size();
    fw_printf("\nSquirrelBIOS s390x\n");
    fw_printf("mem: %lu MB, firmware at 0x%lx\n", (unsigned long)(ram_size >> 20),
              (unsigned long)fw_base);
    css_scan();
    fw_printf("css: %d subchannels\n", ndevs);
    try_boot();
    fw_prompt(cmds);
    halt_quiet();
}
