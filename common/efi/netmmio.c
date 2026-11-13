/* SquirrelBIOS UEFI core (common/efi): the platform side of the network
 * stack for the device-tree machines. Original code, from the VIRTIO 1.1
 * MMIO transport (section 4.2, with the legacy layout of 4.2.4).
 *
 * Every "virtio,mmio" transport with a network device (ID 1) behind it
 * is handed to net_virtio.c; version 1 (QEMU's default, legacy) and
 * version 2 registers both work. Time comes from the architecture's
 * counter. Named netmmio.c, not net_*.c, so the x86_64 build, which takes
 * common/efi/net_*.c, leaves it out. */
#include "net.h"

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
#define VM_STATUS        0x070
#define VM_QUEUE_DESC    0x080
#define VM_QUEUE_DRIVER  0x090
#define VM_QUEUE_DEVICE  0x0a0
#define VM_CONFIG        0x100

#define VIRT_MAGIC       0x74726976 /* "virt" */
#define DEVICE_NET       1

/* The same hardware vendor node as virtio_mmio.c's disks: GUID, base */
#define NET_MMIO_GUID \
    { 0x5371b1d3, 0x6d2c, 0x4f0e, { 0x9a, 0x38, 0x51, 0x75, 0x69, 0x72, 0x74, 0x6d } }

#if defined(__aarch64__)
const u16 net_client_arch = 11;     /* arm64 UEFI */
#else
const u16 net_client_arch = 27;     /* riscv64 UEFI */
#endif

u64 net_ms(void)
{
    u64 hz = arch_counter_hz();
    return arch_counter() / (hz >= 1000 ? hz / 1000 : 1);
}

struct vio_mmio {
    struct vnet_transport t;
    uptr base;
    u32 version;
};

static uptr B(void *ctx) { return ((struct vio_mmio *)ctx)->base; }
static u32 rd(void *ctx, u32 off) { return mmio_read32(B(ctx) + off); }
static void wr(void *ctx, u32 off, u32 v) { mmio_write32(B(ctx) + off, v); }

static void wr64(void *ctx, u32 off, u64 v)
{
    wr(ctx, off, (u32)v);
    wr(ctx, off + 4, (u32)(v >> 32));
}

static u32 m_features(void *ctx, int word)
{
    wr(ctx, VM_DEV_FEAT_SEL, (u32)word);
    return rd(ctx, VM_DEV_FEAT);
}

static void m_set_features(void *ctx, int word, u32 v)
{
    wr(ctx, VM_DRV_FEAT_SEL, (u32)word);
    wr(ctx, VM_DRV_FEAT, v);
}

static u8 m_get_status(void *ctx) { return (u8)rd(ctx, VM_STATUS); }
static void m_set_status(void *ctx, u8 s) { wr(ctx, VM_STATUS, s); }
static u8 m_config8(void *ctx, u32 off) { return *(volatile u8 *)(B(ctx) + VM_CONFIG + off); }

static u16 m_queue_max(void *ctx, u16 q)
{
    wr(ctx, VM_QUEUE_SEL, q);
    u32 n = rd(ctx, VM_QUEUE_MAX);
    return n > 0xFFFF ? 0xFFFF : (u16)n;
}

static int m_queue_set(void *ctx, u16 q, u16 size, u64 desc, u64 avail, u64 used)
{
    struct vio_mmio *v = ctx;
    wr(ctx, VM_QUEUE_SEL, q);
    wr(ctx, VM_QUEUE_NUM, size);
    if (v->version == 2) {
        wr64(ctx, VM_QUEUE_DESC, desc);
        wr64(ctx, VM_QUEUE_DRIVER, avail);
        wr64(ctx, VM_QUEUE_DEVICE, used);
        wr(ctx, VM_QUEUE_READY, 1);
    } else {
        if ((desc & 4095) || (desc >> 44))
            return 0;
        wr(ctx, VM_GUEST_PAGE, 4096);
        wr(ctx, VM_QUEUE_ALIGN, 4096);
        wr(ctx, VM_QUEUE_PFN, (u32)(desc >> 12));
    }
    return 1;
}

static void m_notify(void *ctx, u16 q) { wr(ctx, VM_QUEUE_NOTIFY, q); }

static EFI_DEVICE_PATH_PROTOCOL *mmio_path(u64 b)
{
    static const EFI_GUID g = NET_MMIO_GUID;
    u8 *p;
    if (efi_allocate_pool(EfiBootServicesData, 32, (void **)&p) != EFI_SUCCESS)
        return NULL;
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
    return (EFI_DEVICE_PATH_PROTOCOL *)p;
}

static int probe_one(uptr b)
{
    if (mmio_read32(b + VM_MAGIC) != VIRT_MAGIC || mmio_read32(b + VM_DEVICE_ID) != DEVICE_NET)
        return 0;
    u32 version = mmio_read32(b + VM_VERSION);
    if (version != 1 && version != 2)
        return 0;
    struct vio_mmio *v;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *v, (void **)&v) != EFI_SUCCESS)
        return 0;
    memset(v, 0, sizeof *v);
    v->base = b;
    v->version = version;
    v->t.ctx = v;
    v->t.features = m_features;
    v->t.set_features = m_set_features;
    v->t.get_status = m_get_status;
    v->t.set_status = m_set_status;
    v->t.config8 = m_config8;
    v->t.queue_max = m_queue_max;
    v->t.queue_set = m_queue_set;
    v->t.notify = m_notify;
    /* version 2 needs VIRTIO_F_VERSION_1 (feature bit 32) */
    v->t.version1 = version == 2 && (m_features(v, 1) & 1);
    if (version == 2 && !v->t.version1) {
        efi_free_pool(v);
        return 0;
    }
    struct net_dev *nd = vnet_init(&v->t);
    if (!nd) {
        printf("net: virtio-net at 0x%016lx did not start\n", (u64)b);
        efi_free_pool(v);
        return 0;
    }
    char what[48] = "virtio-net at 0x0000000000000000 (mmio v0)";
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
        what[16 + i] = hex[((u64)b >> (60 - 4 * i)) & 15];
    what[40] = (char)('0' + version);
    net_add(nd, mmio_path(b), what);
    return 1;
}

int net_platform_probe(void)
{
    int n = 0;
    for (int node = fdt_find_compatible("virtio,mmio", -1); node >= 0;
         node = fdt_find_compatible("virtio,mmio", node)) {
        u64 b, size;
        if (fdt_reg(node, 0, &b, &size))
            n += probe_one((uptr)b);
    }
    return n;
}
