/* SquirrelBIOS UEFI core (common/efi): SATA disks on an AHCI controller.
 * Original code, from the Serial ATA AHCI 1.3.1 specification (HBA and
 * port registers, command list, FIS) and ATA/ATAPI-8 ACS (IDENTIFY
 * DEVICE, READ/WRITE DMA EXT).
 *
 * Every implemented port with an ATA disk behind it is a disk; ATAPI and
 * empty ports are left alone. One command slot per port, polled, with a
 * page-aligned bounce buffer of BOUNCE bytes in one PRD entry. Disks must
 * use 512-byte logical sectors and LBA48. */
#include "efi.h"

#define ABAR_BAR    5

#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_PI      0x0C
#define GHC_AE      (1u << 31)
#define CAP_SSS     (1u << 27)

#define PORT(n)     (0x100 + 0x80 * (n))
#define P_CLB       0x00
#define P_FB        0x08
#define P_IS        0x10
#define P_CMD       0x18
#define P_TFD       0x20
#define P_SIG       0x24
#define P_SSTS      0x28
#define P_SERR      0x30
#define P_CI        0x38

#define CMD_ST      (1u << 0)
#define CMD_SUD     (1u << 1)
#define CMD_POD     (1u << 2)
#define CMD_FRE     (1u << 4)
#define CMD_FR      (1u << 14)
#define CMD_CR      (1u << 15)
#define TFD_ERR     0x01
#define TFD_DRQ     0x08
#define TFD_BSY     0x80
#define IS_TFES     (1u << 30)
#define SIG_ATA     0x00000101

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35

#define BOUNCE      (64u * 1024)
#define SPINS       100000000ULL
#define MSG_SATA_DP 0x12

struct sata {
    struct sq_blk blk;          /* first */
    uptr port;                  /* its registers */
    u8 *mem;                    /* command list, received FIS, table */
    u8 *bounce;
};

static int wait_clear(uptr reg, u32 bits)
{
    for (u64 i = 0; i < SPINS; i++)
        if (!(mmio_read32(reg) & bits))
            return 1;
    return 0;
}

/* One command in slot 0: an H2D register FIS, bytes to or from the
 * bounce buffer */
static int command(struct sata *s, u8 op, u64 lba, u32 count, u32 bytes, int write)
{
    u8 *cl = s->mem, *table = s->mem + 2048;
    memset(table, 0, 0x90);
    u8 *fis = table;
    fis[0] = 0x27;                          /* register, host to device */
    fis[1] = 0x80;                          /* a command */
    fis[2] = op;
    fis[4] = (u8)lba;
    fis[5] = (u8)(lba >> 8);
    fis[6] = (u8)(lba >> 16);
    fis[7] = 0x40;                          /* LBA */
    fis[8] = (u8)(lba >> 24);
    fis[9] = (u8)(lba >> 32);
    fis[10] = (u8)(lba >> 40);
    fis[12] = (u8)count;
    fis[13] = (u8)(count >> 8);
    u64 dba = (uptr)s->bounce;
    memcpy(table + 0x80, &dba, 8);
    u32 dbc = bytes - 1;
    memcpy(table + 0x8C, &dbc, 4);

    memset(cl, 0, 32);
    u32 dw0 = 5 | (write ? 1u << 6 : 0) | 1u << 16;     /* 5-dword FIS, one PRD */
    memcpy(cl, &dw0, 4);
    u64 ctba = (uptr)table;
    memcpy(cl + 8, &ctba, 8);

    if (!wait_clear(s->port + P_TFD, TFD_BSY | TFD_DRQ))
        return 0;
    mmio_write32(s->port + P_IS, 0xFFFFFFFF);
    __sync_synchronize();
    mmio_write32(s->port + P_CI, 1);
    for (u64 i = 0; i < SPINS; i++) {
        if (mmio_read32(s->port + P_IS) & IS_TFES)
            return 0;
        if (!(mmio_read32(s->port + P_CI) & 1))
            return !(mmio_read32(s->port + P_TFD) & TFD_ERR);
    }
    return 0;
}

static int sata_rw(struct sata *s, int write, u64 lba, u8 *buf, u32 bytes)
{
    while (bytes) {
        u32 n = bytes > BOUNCE ? BOUNCE : bytes;
        if (write)
            memcpy(s->bounce, buf, n);
        if (!command(s, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, n / 512, n, write))
            return 0;
        if (!write)
            memcpy(buf, s->bounce, n);
        lba += n / 512;
        buf += n;
        bytes -= n;
    }
    return 1;
}

static int sata_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return sata_rw((struct sata *)b, 0, lba, buf, bytes);
}

static int sata_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return sata_rw((struct sata *)b, 1, lba, buf, bytes);
}

/* Pci(..)/Sata(port, no multiplier, LUN 0) */
static EFI_DEVICE_PATH_PROTOCOL *sata_path(EFI_DEVICE_PATH_PROTOCOL *pci, u16 port)
{
    u8 node[14] = {
        MESSAGING_DEVICE_PATH, MSG_SATA_DP, 10, 0, (u8)port, (u8)(port >> 8), 0xFF, 0xFF, 0, 0,
        END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0
    };
    return efi_dp_append(pci, (const EFI_DEVICE_PATH_PROTOCOL *)node);
}

/* The port's command engine and FIS receive off */
static int port_stop(uptr p)
{
    mmio_write32(p + P_CMD, mmio_read32(p + P_CMD) & ~CMD_ST);
    if (!wait_clear(p + P_CMD, CMD_CR))
        return 0;
    mmio_write32(p + P_CMD, mmio_read32(p + P_CMD) & ~CMD_FRE);
    return wait_clear(p + P_CMD, CMD_FR);
}

/* Stop the port's engines, give it our memory, start it again */
static int port_start(struct sata *s, int spin_up)
{
    uptr p = s->port;
    if (!port_stop(p))
        return 0;
    u64 clb = (uptr)s->mem, fb = (uptr)s->mem + 1024;
    mmio_write32(p + P_CLB, (u32)clb);
    mmio_write32(p + P_CLB + 4, (u32)(clb >> 32));
    mmio_write32(p + P_FB, (u32)fb);
    mmio_write32(p + P_FB + 4, (u32)(fb >> 32));
    mmio_write32(p + P_SERR, 0xFFFFFFFF);
    mmio_write32(p + P_IS, 0xFFFFFFFF);
    u32 cmd = mmio_read32(p + P_CMD) | CMD_FRE | CMD_POD | (spin_up ? CMD_SUD : 0);
    mmio_write32(p + P_CMD, cmd);
    if (!wait_clear(p + P_TFD, TFD_BSY | TFD_DRQ))
        return 0;
    mmio_write32(p + P_CMD, cmd | CMD_ST);
    return 1;
}

static int port_add(struct sq_pci *pci, uptr abar, u32 n, int spin_up)
{
    uptr p = abar + PORT(n);
    if ((mmio_read32(p + P_SSTS) & 0xF) != 3)
        return 0;                           /* nothing attached */
    struct sata *s;
    EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;    /* below 4 GB: HBAs without 64-bit DMA */
    if (efi_allocate_pool(EfiBootServicesData, sizeof *s, (void **)&s) != EFI_SUCCESS)
        return 0;
    if (efi_allocate_pages(AllocateMaxAddress, EfiBootServicesData, 1 + BOUNCE / 4096, &a) != EFI_SUCCESS)
        return 0;
    memset(s, 0, sizeof *s);
    s->port = p;
    s->mem = (u8 *)(uptr)a;
    s->bounce = s->mem + 4096;
    memset(s->mem, 0, 4096);
    if (!port_start(s, spin_up)) {
        printf("disk: AHCI port %u did not start\n", n);
        return 0;
    }
    /* The device's first D2H FIS (once FIS receive is on) gives its
     * signature: leave ATAPI and port multipliers alone */
    if (mmio_read32(p + P_SIG) != SIG_ATA) {
        port_stop(p);
        return 0;
    }
    if (!command(s, ATA_IDENTIFY, 0, 0, 512, 0)) {
        printf("disk: AHCI port %u did not answer IDENTIFY\n", n);
        return 0;
    }
    const u16 *id = (const u16 *)s->bounce;
    u64 sectors = 0;
    if (id[83] & (1u << 10))                /* LBA48 */
        for (int i = 3; i >= 0; i--)
            sectors = sectors << 16 | id[100 + i];
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12)))
        sectors = 0;                        /* sectors larger than 512 bytes */
    if (!sectors) {
        printf("disk: AHCI port %u: no LBA48 512-byte disk\n", n);
        return 0;
    }
    s->blk.sectors = sectors;
    s->blk.path = sata_path(pci->path, (u16)n);
    s->blk.read = sata_read;
    s->blk.write = sata_write;
    printf("disk: AHCI at PCI %02x:%02x.%x port %u, %lu sectors\n",
           pci->bus, pci->dev, pci->fn, n, sectors);
    efi_disk_add(&s->blk);
    return 1;
}

int ahci_add(struct sq_pci *pci)
{
    uptr abar = (uptr)pci->bar[ABAR_BAR];
    if (!abar)
        return 0;
    mmio_write32(abar + HBA_GHC, mmio_read32(abar + HBA_GHC) | GHC_AE);
    u32 pi = mmio_read32(abar + HBA_PI);
    int spin_up = (mmio_read32(abar + HBA_CAP) & CAP_SSS) != 0;
    int found = 0;
    for (u32 n = 0; n < 32; n++)
        if (pi & (1u << n))
            found += port_add(pci, abar, n, spin_up);
    return found;
}
