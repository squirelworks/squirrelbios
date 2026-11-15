/* SquirrelBIOS UEFI core (common/efi): SD cards behind a standard SD
 * host controller, as Block I/O. Original code, written from the SD
 * Association's simplified specifications (SD Host Controller 3.00 and
 * the Physical Layer). Polled, programmed I/O, one slot per controller:
 *
 *   - on PCI (class 08 05, QEMU's sdhci-pci), from pci_disk_add;
 *   - at an MMIO address the platform found (sdhci_add_mmio; sdhci_dt.c
 *     takes them from the device tree).
 *
 * Every register access is 32 bits wide (the Broadcom controllers take
 * nothing smaller), so the transfer mode and command, which share a word,
 * are written together. Cards: SD v1 (byte addresses) and v2 SDSC/SDHC/
 * SDXC (block addresses), 1-bit bus at up to 25 MHz. Reads and writes of
 * several blocks use CMD18/CMD25 with Auto CMD12. Not handled: MMC and
 * eMMC (CMD1 initialisation), UHS signalling, DMA. */
#include "efi.h"

#define R_BLKSIZE   0x04            /* block size, block count */
#define R_ARG       0x08
#define R_CMD       0x0C            /* transfer mode (low half), command */
#define R_RESP      0x10            /* 4 words */
#define R_DATA      0x20
#define R_PRESENT   0x24
#define R_HOSTCTL   0x28            /* host control 1, power, gap, wakeup */
#define R_CLOCK     0x2C            /* clock control, timeout, software reset */
#define R_STATUS    0x30            /* normal (low), error (high) */
#define R_STATUS_EN 0x34
#define R_SIGNAL_EN 0x38
#define R_CAPS      0x40
#define R_VERSION   0xFC            /* host controller version in the high half */

#define PS_CMD_INHIBIT  (1u << 0)
#define PS_DAT_INHIBIT  (1u << 1)
#define PS_CARD         (1u << 16)

#define ST_CMD_DONE     (1u << 0)
#define ST_XFER_DONE    (1u << 1)
#define ST_WRITE_READY  (1u << 4)
#define ST_READ_READY   (1u << 5)
#define ST_ERROR        (1u << 15)

#define RESET_ALL       (1u << 24)
#define RESET_CMD       (1u << 25)
#define RESET_DAT       (1u << 26)

/* command register (upper half of R_CMD) */
#define RSP_NONE    0x00
#define RSP_136     0x01
#define RSP_48      0x02
#define RSP_48_BUSY 0x03
#define C_CRC       0x08
#define C_INDEX     0x10
#define C_DATA      0x20
#define R1   (RSP_48 | C_CRC | C_INDEX)
#define R1B  (RSP_48_BUSY | C_CRC | C_INDEX)
#define R2   (RSP_136 | C_CRC)
#define R3   RSP_48
#define R6   (RSP_48 | C_CRC | C_INDEX)
#define R7   (RSP_48 | C_CRC | C_INDEX)

/* transfer mode (lower half of R_CMD) */
#define TM_COUNT    (1u << 1)
#define TM_ACMD12   (1u << 2)
#define TM_READ     (1u << 4)
#define TM_MULTI    (1u << 5)

#define MAX_BLOCKS  0xFFFF

struct sdhci {
    struct sq_blk blk;
    uptr regs;
    u32 rca;
    int block_addr;                 /* SDHC/SDXC: the argument is a block number */
    const char *where;
};

static u32 rd(struct sdhci *s, u32 off) { return mmio_read32(s->regs + off); }
static void wr(struct sdhci *s, u32 off, u32 v) { mmio_write32(s->regs + off, v); }

static int wait_clear(struct sdhci *s, u32 off, u32 bits, u32 ms)
{
    u64 end = arch_counter() + efi_counter_hz() / 1000 * ms;
    while (rd(s, off) & bits)
        if (arch_counter() > end)
            return 0;
    return 1;
}

/* Waits for one of bits in the normal status (clearing it), or an error */
static int wait_status(struct sdhci *s, u32 bits, u32 ms)
{
    u64 end = arch_counter() + efi_counter_hz() / 1000 * ms;
    for (;;) {
        u32 st = rd(s, R_STATUS);
        if (st & ST_ERROR) {
            wr(s, R_STATUS, st & 0xFFFF8000u);
            return 0;
        }
        if (st & bits) {
            wr(s, R_STATUS, st & bits);
            return 1;
        }
        if (arch_counter() > end)
            return 0;
    }
}

static void reset_lines(struct sdhci *s, u32 which)
{
    wr(s, R_CLOCK, rd(s, R_CLOCK) | which);
    wait_clear(s, R_CLOCK, which, 100);
}

/* Sends a command; mode is the transfer mode for data commands */
static int cmd(struct sdhci *s, u32 index, u32 arg, u32 flags, u32 mode)
{
    u32 inhibit = PS_CMD_INHIBIT | ((flags & C_DATA) || (flags & 3) == RSP_48_BUSY ? PS_DAT_INHIBIT : 0);
    if (!wait_clear(s, R_PRESENT, inhibit, 1000))
        return 0;
    wr(s, R_STATUS, 0xFFFFFFFF);
    wr(s, R_ARG, arg);
    wr(s, R_CMD, (index << 8 | flags) << 16 | mode);
    if (!wait_status(s, ST_CMD_DONE, 1000)) {
        reset_lines(s, RESET_CMD | RESET_DAT);
        return 0;
    }
    if ((flags & 3) == RSP_48_BUSY && !wait_status(s, ST_XFER_DONE, 1000))
        return 0;
    return 1;
}

static int acmd(struct sdhci *s, u32 index, u32 arg, u32 flags)
{
    return cmd(s, 55, s->rca << 16, R1, 0) && cmd(s, index, arg, flags, 0);
}

/* SD clock at most hz from the base clock (MHz): 10-bit divider (v3) or
 * a power of two (v1, v2) */
static int set_clock(struct sdhci *s, u32 base_mhz, u32 hz, u32 version)
{
    u32 base = base_mhz * 1000000u, div = 0;
    if (base > hz) {
        div = (base + 2 * hz - 1) / (2 * hz);       /* f = base / (2 * div) */
        if (version < 2) {
            u32 p = 1;
            while (p < div && p < 0x80)
                p <<= 1;
            div = p;
        } else if (div > 0x3FF)
            div = 0x3FF;
    }
    u32 c = rd(s, R_CLOCK) & 0xFFFF0000u;
    wr(s, R_CLOCK, c);                              /* clocks off */
    c |= (div & 0xFF) << 8 | ((div >> 8) & 3) << 6 | 1;  /* internal clock on */
    wr(s, R_CLOCK, c);
    u64 end = arch_counter() + efi_counter_hz() / 10;
    while (!(rd(s, R_CLOCK) & 2))                   /* stable */
        if (arch_counter() > end)
            return 0;
    wr(s, R_CLOCK, c | 4);                          /* SD clock on */
    return 1;
}

/* CSD bit k (as the card sends it) is response bit k - 8: the controller
 * drops the CRC byte of a 136-bit response */
static u32 csd_bits(const u32 *r, int hi, int lo)
{
    u32 v = 0;
    for (int k = hi; k >= lo; k--) {
        int b = k - 8;
        v = v << 1 | ((r[b / 32] >> (b % 32)) & 1);
    }
    return v;
}

static int xfer(struct sdhci *s, int write, u64 lba, u8 *buf, u32 bytes)
{
    while (bytes) {
        u32 n = bytes / 512;
        if (n > MAX_BLOCKS)
            n = MAX_BLOCKS;
        u32 arg = s->block_addr ? (u32)lba : (u32)(lba * 512);
        if (lba + n > 0xFFFFFFFFull || (!s->block_addr && lba * 512 + (u64)n * 512 > 0xFFFFFFFFull))
            return 0;
        wr(s, R_BLKSIZE, n << 16 | 512);
        u32 mode = TM_COUNT | (write ? 0 : TM_READ) | (n > 1 ? TM_MULTI | TM_ACMD12 : 0);
        u32 index = write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
        if (!cmd(s, index, arg, R1 | C_DATA, mode))
            return 0;
        for (u32 i = 0; i < n; i++, buf += 512) {
            if (!wait_status(s, write ? ST_WRITE_READY : ST_READ_READY, 1000))
                goto fail;
            for (int w = 0; w < 512; w += 4) {
                if (write) {
                    u32 v;
                    memcpy(&v, buf + w, 4);
                    wr(s, R_DATA, v);
                } else {
                    u32 v = rd(s, R_DATA);
                    memcpy(buf + w, &v, 4);
                }
            }
        }
        if (!wait_status(s, ST_XFER_DONE, 2000))
            goto fail;
        lba += n;
        bytes -= n * 512;
    }
    return 1;
fail:
    reset_lines(s, RESET_CMD | RESET_DAT);
    return 0;
}

static int sd_read(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return xfer((struct sdhci *)b, 0, lba, buf, bytes);
}

static int sd_write(struct sq_blk *b, u64 lba, void *buf, u32 bytes)
{
    return xfer((struct sdhci *)b, 1, lba, buf, bytes);
}

/* Power, clock and the card's identification; 0 when there is no usable
 * SD card. base_mhz overrides the capabilities' base clock when not 0. */
static int sd_start(struct sdhci *s, u32 base_mhz)
{
    u32 version = (rd(s, R_VERSION) >> 16) & 0xFF;
    u32 caps = rd(s, R_CAPS);
    reset_lines(s, RESET_ALL);
    if (!base_mhz)
        base_mhz = (caps >> 8) & (version >= 2 ? 0xFF : 0x3F);
    if (!base_mhz)
        base_mhz = 50;                              /* not reported: a guess */
    if (!(rd(s, R_PRESENT) & PS_CARD)) {
        printf("disk: SD host at %s: no card\n", s->where);
        return 0;
    }
    /* 3.3 V (or the highest the capabilities offer), bus power on */
    u32 volt = caps & (1u << 24) ? 7 : caps & (1u << 25) ? 6 : 5;
    wr(s, R_HOSTCTL, (rd(s, R_HOSTCTL) & 0xFFFF00FFu) | (volt << 1 | 1) << 8);
    wr(s, R_CLOCK, (rd(s, R_CLOCK) & 0xFF00FFFFu) | 0xE << 16);     /* data timeout */
    wr(s, R_STATUS_EN, 0xFFFFFFFF);                 /* every status latches */
    wr(s, R_SIGNAL_EN, 0);                          /* no interrupts: polled */
    if (!set_clock(s, base_mhz, 400000, version))
        return 0;
    efi_stall(2000);                                /* 74 clocks and more */

    cmd(s, 0, 0, RSP_NONE, 0);                      /* GO_IDLE_STATE */
    int v2 = cmd(s, 8, 0x1AA, R7, 0) && (rd(s, R_RESP) & 0xFFF) == 0x1AA;
    s->rca = 0;
    u32 ocr = 0;
    u64 end = arch_counter() + efi_counter_hz();     /* a card may take 1 s */
    do {
        if (!acmd(s, 41, (v2 ? 1u << 30 : 0) | 0x00FF8000u, R3)) {
            printf("disk: SD host at %s: the card is not an SD memory card\n", s->where);
            return 0;
        }
        ocr = rd(s, R_RESP);
        if (ocr & (1u << 31))
            break;
        efi_stall(1000);
    } while (arch_counter() < end);
    if (!(ocr & (1u << 31))) {
        printf("disk: SD host at %s: the card stays busy\n", s->where);
        return 0;
    }
    s->block_addr = v2 && (ocr & (1u << 30));
    if (!cmd(s, 2, 0, R2, 0) || !cmd(s, 3, 0, R6, 0))       /* ALL_SEND_CID, SEND_RELATIVE_ADDR */
        return 0;
    s->rca = rd(s, R_RESP) >> 16;
    if (!cmd(s, 9, s->rca << 16, R2, 0))                    /* SEND_CSD */
        return 0;
    u32 r[4];
    for (int i = 0; i < 4; i++)
        r[i] = rd(s, R_RESP + 4 * i);
    u64 sectors;
    if (csd_bits(r, 127, 126) == 1)                         /* CSD 2.0 */
        sectors = ((u64)csd_bits(r, 69, 48) + 1) * 1024;
    else {
        u64 blocks = ((u64)csd_bits(r, 73, 62) + 1) << (csd_bits(r, 49, 47) + 2);
        sectors = (blocks << csd_bits(r, 83, 80)) / 512;
    }
    if (!cmd(s, 7, s->rca << 16, R1B, 0) ||                 /* SELECT_CARD */
        (!s->block_addr && !cmd(s, 16, 512, R1, 0)))        /* SET_BLOCKLEN */
        return 0;
    set_clock(s, base_mhz, 25000000, version);              /* default speed */
    s->blk.sectors = sectors;
    s->blk.read = sd_read;
    s->blk.write = sd_write;
    printf("disk: SD card at %s (%s, RCA %04x), %lu sectors\n", s->where,
           s->block_addr ? "SDHC/SDXC" : v2 ? "SDSC v2" : "SDSC v1", s->rca, sectors);
    return 1;
}

static struct sdhci *alloc(uptr regs)
{
    struct sdhci *s;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *s, (void **)&s) != EFI_SUCCESS)
        return NULL;
    memset(s, 0, sizeof *s);
    s->regs = regs;
    return s;
}

/* Pci(..)/SD(slot) */
static EFI_DEVICE_PATH_PROTOCOL *sd_path(EFI_DEVICE_PATH_PROTOCOL *parent)
{
    static const u8 node[9] = {
        MESSAGING_DEVICE_PATH, 26, 5, 0, 0,             /* MSG_SD_DP, slot 0 */
        END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4, 0
    };
    return efi_dp_append(parent, (const EFI_DEVICE_PATH_PROTOCOL *)node);
}

int sdhci_add(struct sq_pci *p)
{
    uptr regs = (uptr)p->bar[0];
    struct sdhci *s = regs ? alloc(regs) : NULL;
    if (!s)
        return 0;
    static char where[8][16];
    static int nwhere;
    char *w = where[nwhere++ & 7];
    const char *hex = "0123456789abcdef";
    memcpy(w, "PCI 00:00.0", 12);
    w[4] = hex[p->bus >> 4];
    w[5] = hex[p->bus & 0xF];
    w[7] = hex[p->dev >> 4];
    w[8] = hex[p->dev & 0xF];
    w[10] = hex[p->fn & 7];
    s->where = w;
    if (!sd_start(s, 0))
        return 0;
    s->blk.path = sd_path(p->path);
    efi_disk_add(&s->blk);
    return 1;
}

/* A controller the platform found (sdhci_dt.c: the device tree):
 * registers at regs, path its device path; base_mhz as for sd_start */
int sdhci_add_mmio(uptr regs, u32 base_mhz, EFI_DEVICE_PATH_PROTOCOL *path, const char *where)
{
    struct sdhci *s = alloc(regs);
    if (!s)
        return 0;
    s->where = where;
    if (!sd_start(s, base_mhz))
        return 0;
    s->blk.path = sd_path(path);
    efi_disk_add(&s->blk);
    return 1;
}
