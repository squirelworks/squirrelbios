/* Smoke-test kernel for SquirrelBIOS m68k-virt: stands in for an m68k
 * Linux. Checks the hand-off (supervisor mode, interrupts masked, MMU
 * off, VBR 0) and the bootinfo records after its own end (machine, memory,
 * devices, the command line, no ramdisk record), then powers off through
 * the virt ctrl device. Original code. */
#include "fwlib.h"

uint32_t get_sr(void), get_vbr(void), get_tc(void);
extern char _end[];

static uint32_t tty, ctrl;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void fw_putc(char c)
{
    if (tty)
        *(volatile uint32_t *)tty = (uint8_t)c;
}

int fw_getc(void)
{
    return -1;
}

static const char *ok(int good)
{
    return good ? "ok" : "bad";
}

void kmain(void)
{
    uint32_t sr = get_sr();
    int state = (sr & 0x2000) && (sr & 0x0700) == 0x0700 && get_vbr() == 0 &&
                !(get_tc() & 0x8000);

    const uint8_t *bi = (const uint8_t *)(((uint32_t)_end + 1) & ~1u);
    uint32_t mach = 0, mem_base = ~0u, mem_size = 0, ramdisk = 0, records = 0;
    const char *cmdline = 0;
    int ended = 0;
    for (uint32_t off = 0; off < 4096;) {
        uint16_t tag = (uint16_t)(bi[off] << 8 | bi[off + 1]);
        uint16_t size = (uint16_t)(bi[off + 2] << 8 | bi[off + 3]);
        const uint8_t *d = bi + off + 4;
        if (tag == 0) {
            ended = 1;
            break;
        }
        if (size < 4)
            break;
        records++;
        if (tag == 0x0001) mach = rd32(d);
        if (tag == 0x0005) { mem_base = rd32(d); mem_size = rd32(d + 4); }
        if (tag == 0x0006) ramdisk = 1;
        if (tag == 0x0007) cmdline = (const char *)d;
        if (tag == 0x8003) tty = rd32(d);
        if (tag == 0x8005) ctrl = rd32(d);
        off += size;
    }
    int mem = mem_base == 0 && mem_size >= (32u << 20);
    int cl = cmdline && !strcmp(cmdline, "console=ttyGF0 squirrel");

    fw_printf("OS-ENTRY m68k state=%s bootinfo=%s mach=%u mem=%s devices=%s cmdline=%s "
              "ramdisk=%s\n", ok(state), ok(ended && records >= 8), mach, ok(mem),
              ok(tty && ctrl), ok(cl), ok(!ramdisk));
    fw_printf("OS-DONE m68k, powering off\n");
    if (ctrl)
        *(volatile uint32_t *)(ctrl + 4) = 2;       /* halt */
    for (;;)
        ;
}
