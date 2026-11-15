/* SquirrelBIOS make test: s390x test kernel. The firmware IPLs it from a
 * virtio-blk-ccw disk (an ELF linked at 0x10000, like s390x Linux). It
 * checks the hand-off, prints through the SCLP (the firmware has set the
 * VT220 event mask) and stops the machine. Built with LINUX_IMAGE it is
 * a Linux-style boot image instead (tests/s390x-image.S): it checks the
 * parameter area, the initrd and the command line. Original code. */
#include "fwlib.h"

extern struct { uint64_t r2, r3, r15; uint32_t mask_hi, mask_lo; } entry_regs;

/* The image is bigger than one 64 KB read: the firmware has to read it
 * in several requests */
uint32_t big[50000] __attribute__((used)) = { [0] = 0x5351u, [24999] = 0x1234u, [49999] = 0x5a5au };
static uint32_t zeroed[1024];

static uint8_t sccb[4096] __attribute__((aligned(4096)));
static char line[200];
static int len;

static void sclp_write(void)
{
    uint32_t cmd = 0x00760005u;
    uint64_t addr = (uint64_t)sccb;
    memset(sccb, 0, 16);
    *(uint16_t *)sccb = 8 + 6 + len;
    *(uint16_t *)(sccb + 8) = 6 + len;
    sccb[10] = 0x1a;                    /* VT220 */
    memcpy(sccb + 14, line, len);
    len = 0;
    /* QEMU answers at once; the service signal stays pending, unused */
    asm volatile(".insn rre,0xb2200000,%0,%1" :: "d"(cmd), "a"(addr) : "cc", "memory");
}

void fw_putc(char c)
{
    line[len++] = c;
    if (c == '\n' || len == (int)sizeof(line))
        sclp_write();
}

int fw_getc(void) { return -1; }

void kernel_main(void)
{
    uint32_t schid;
    asm volatile("l %0,0xb8" : "=d"(schid));
    /* 64-bit addressing, no DAT, I/O, external or machine-check
     * interrupts, key 0, supervisor state */
    int state = (entry_regs.mask_hi & 0xffff0000u) == 0 && (entry_regs.mask_hi & 1) &&
                entry_regs.mask_lo == 0x80000000u;
    int mem = entry_regs.r2 >= (64ul << 20) && !(entry_regs.r2 & 0xfffff);
    int ids = (schid & 0xffff0000u) == 0x00010000u;
    const volatile uint32_t *b = big;
    int data = b[0] == 0x5351u && b[24999] == 0x1234u && b[49999] == 0x5a5au && b[1] == 0;
    int bss = 1;
    for (unsigned i = 0; i < 1024; i++)
        if (zeroed[i])
            bss = 0;
    fw_printf("OS-ENTRY s390x state=%s mem=%luMB schid=%s data=%s bss=%s\n",
              state ? "ok" : "bad", (unsigned long)(entry_regs.r2 >> 20),
              ids ? "ok" : "bad", data ? "ok" : "bad", bss ? "ok" : "bad");
    if (!state)
        fw_printf("psw mask 0x%08x%08x =bad\n", entry_regs.mask_hi, entry_regs.mask_lo);
    if (!mem)
        fw_printf("r2 0x%lx =bad\n", (unsigned long)entry_regs.r2);
#ifdef LINUX_IMAGE
    /* The parameter area: the initrd (the second disk: a marker, then
     * its size again) and the command line */
    extern const volatile uint64_t image_parm[];
    extern const char image_cmdline[];
    const volatile uint8_t *rd = (const volatile uint8_t *)image_parm[1];
    uint64_t rd_size = image_parm[2];
    int initrd = rd_size >= 512 && image_parm[1] >= 0x100000 && !(image_parm[1] & 0xfff) &&
                 rd[0] == 'S' && rd[1] == 'Q' && rd[2] == 'R' && rd[3] == 'D' &&
                 rd[rd_size - 1] == 0x5a;
    fw_printf("LINUX-IMAGE s390x initrd=%s cmdline=\"%s\"\n", initrd ? "ok" : "bad",
              image_cmdline);
#endif
    fw_printf("OS-DONE s390x\n");
    /* Disabled wait at 0xfff: QEMU shuts down */
    static uint64_t psw[2] __attribute__((aligned(8))) = { 0x0002000180000000ul, 0xfff };
    asm volatile("lpswe %0" :: "Q"(psw) : "memory");
    for (;;)
        ;
}
