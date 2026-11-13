/* SquirrelBIOS UEFI core (common/efi): QEMU's firmware configuration
 * device (fw_cfg), memory-mapped as on the virt machines. Original code,
 * written from QEMU's documented interface (docs/specs/fw_cfg.rst): the
 * data register at +0, a big-endian 16-bit selector at +8, the DMA
 * address register at +16 (big-endian, the write of its low half starts
 * the transfer), the signature "QEMU" at selector 0, the feature bits at
 * selector 1 (bit 1: DMA), and the file directory at selector 0x19. The
 * device is found through the device tree ("qemu,fw-cfg-mmio"). */
#include "sq.h"

#define FWCFG_SIGNATURE 0x0000
#define FWCFG_ID        0x0001
#define FWCFG_FILE_DIR  0x0019

#define DMA_ERROR  0x01
#define DMA_READ   0x02
#define DMA_SELECT 0x08

static uptr base;
static int dma;

struct dma_access {
    u32 control;
    u32 length;
    u64 address;
} __attribute__((aligned(16)));

static void select(u16 sel)
{
    mmio_write16(base + 8, bswap16(sel));
}

static void read_pio(void *buf, u32 len)
{
    u8 *p = buf;
    while (len--)
        *p++ = mmio_read8(base);
}

/* Read len bytes of the current item (sel < 0) or of item sel, by DMA */
static int read_dma(int sel, void *buf, u32 len)
{
    static struct dma_access d;
    d.control = bswap32((sel >= 0 ? ((u32)sel << 16 | DMA_SELECT) : 0) | DMA_READ);
    d.length = bswap32(len);
    d.address = bswap64((u64)(uptr)buf);
    __asm__ volatile("" ::: "memory");
    mmio_write32(base + 16, bswap32((u32)((u64)(uptr)&d >> 32)));
    mmio_write32(base + 20, bswap32((u32)(uptr)&d));
    u32 c;
    while ((c = bswap32(*(volatile u32 *)&d.control)) & ~DMA_ERROR)
        arch_relax();
    return !(c & DMA_ERROR);
}

void fwcfg_read(u16 sel, void *buf, u32 len)
{
    if (dma && len >= 64 && read_dma(sel, buf, len))
        return;
    select(sel);
    read_pio(buf, len);
}

int fwcfg_present(void)
{
    if (!base)
        return 0;
    char sig[4];
    select(FWCFG_SIGNATURE);
    read_pio(sig, 4);
    return memcmp(sig, "QEMU", 4) == 0;
}

int fwcfg_init(void)
{
    int node = fdt_find_compatible("qemu,fw-cfg-mmio", -1);
    u64 b;
    if (node >= 0 && fdt_reg(node, 0, &b, NULL))
        base = (uptr)b;
#ifdef CONFIG_FWCFG_BASE
    else
        base = CONFIG_FWCFG_BASE;
#endif
    if (!fwcfg_present()) {
        base = 0;
        return 0;
    }
    u8 id[4];
    select(FWCFG_ID);
    read_pio(id, 4);
    dma = (id[0] & 0x02) != 0;      /* little-endian feature bits */
    return 1;
}

int fwcfg_find(const char *name, u16 *sel, u32 *size)
{
    u8 count[4], entry[64];
    if (!base)
        return 0;
    select(FWCFG_FILE_DIR);
    read_pio(count, 4);
    for (u32 n = be32(count); n; n--) {
        read_pio(entry, 64);
        entry[63] = 0;
        if (strcmp((const char *)entry + 8, name) == 0) {
            *size = be32(entry);
            *sel = be16(entry + 4);
            return 1;
        }
    }
    return 0;
}
