/* SquirrelBIOS m68k-virt (roadmap E3): console, bootinfo, the OS image,
 * the prompt and the hand-off, for QEMU's m68k "virt" machine (68040).
 * Original code, written from the m68k Linux boot protocol (bootinfo
 * records), the Goldfish device register lists and the 68040 manual.
 *
 * QEMU's virt machine has no ROM. The firmware is the -kernel ELF; QEMU
 * describes the machine in bootinfo records placed after the firmware's
 * end (memory, CPU, and the Goldfish TTY, RTC and PIC, the virtio-mmio
 * window and the "virt ctrl" power device). The OS image comes as
 * -initrd: an ELF32 m68k executable, placed at its p_paddr. The OS gets
 * the same kind of bootinfo, right after its own image, as Linux expects.
 */
#include "fwlib.h"

#ifndef SQ_VERSION
#define SQ_VERSION "3.0.0-alpha"
#endif

/* --- bootinfo ---------------------------------------------------------- */

#define BI_LAST             0x0000
#define BI_MACHTYPE         0x0001
#define BI_CPUTYPE          0x0002
#define BI_FPUTYPE          0x0003
#define BI_MMUTYPE          0x0004
#define BI_MEMCHUNK         0x0005
#define BI_RAMDISK          0x0006
#define BI_COMMAND_LINE     0x0007
#define BI_RNG_SEED         0x0008
#define BI_VIRT_QEMU_VERSION 0x8000
#define BI_VIRT_GF_PIC_BASE 0x8001
#define BI_VIRT_GF_RTC_BASE 0x8002
#define BI_VIRT_GF_TTY_BASE 0x8003
#define BI_VIRT_VIRTIO_BASE 0x8004
#define BI_VIRT_CTRL_BASE   0x8005

#define MACH_VIRT           14

extern char _start[], _end[];
void fw_enter(uint32_t entry, uint32_t sp) __attribute__((noreturn));

static uint8_t bootinfo[4096];          /* QEMU's records, copied */
static uint32_t bootinfo_len;
static uint32_t ram_base, ram_size, machtype, qemu_version;
static uint32_t tty_base, rtc_base, ctrl_base, pic_base, virtio_base;
static uint32_t rd_addr, rd_size;
static char cmdline[256];

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Walk records: calls fn(tag, data, data_len) for each; returns the
 * total length up to and including BI_LAST, or 0 if malformed */
static uint32_t bi_walk(const uint8_t *bi, uint32_t max,
                        void (*fn)(uint16_t, const uint8_t *, uint32_t))
{
    uint32_t off = 0;
    while (off + 4 <= max) {
        uint16_t tag = (uint16_t)(bi[off] << 8 | bi[off + 1]);
        uint16_t size = (uint16_t)(bi[off + 2] << 8 | bi[off + 3]);
        if (tag == BI_LAST)
            return off + 4;
        if (size < 4 || off + size > max)
            return 0;
        if (fn)
            fn(tag, bi + off + 4, size - 4u);
        off += size;
    }
    return 0;
}

static void bi_take(uint16_t tag, const uint8_t *d, uint32_t len)
{
    switch (tag) {
    case BI_MACHTYPE: machtype = rd32(d); break;
    case BI_MEMCHUNK:
        if (!ram_size) {
            ram_base = rd32(d);
            ram_size = rd32(d + 4);
        }
        break;
    case BI_RAMDISK: rd_addr = rd32(d); rd_size = rd32(d + 4); break;
    case BI_COMMAND_LINE:
        memcpy(cmdline, d, len < sizeof(cmdline) - 1 ? len : sizeof(cmdline) - 1);
        break;
    case BI_VIRT_QEMU_VERSION: qemu_version = rd32(d); break;
    case BI_VIRT_GF_PIC_BASE: pic_base = rd32(d); break;
    case BI_VIRT_GF_RTC_BASE: rtc_base = rd32(d); break;
    case BI_VIRT_GF_TTY_BASE: tty_base = rd32(d); break;
    case BI_VIRT_VIRTIO_BASE: virtio_base = rd32(d); break;
    case BI_VIRT_CTRL_BASE: ctrl_base = rd32(d); break;
    }
    (void)len;
}

/* --- devices ----------------------------------------------------------- */

static inline void w32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

static inline uint32_t r32(uint32_t a)
{
    return *(volatile uint32_t *)a;
}

/* Goldfish TTY */
#define TTY_PUT_CHAR        0x00
#define TTY_BYTES_READY     0x04
#define TTY_CMD             0x08
#define TTY_DATA_PTR        0x10
#define TTY_DATA_LEN        0x14
#define TTY_CMD_READ_BUFFER 3

void fw_putc(char c)
{
    if (tty_base)       /* the TTY's host side turns \n into \r\n */
        w32(tty_base + TTY_PUT_CHAR, (uint8_t)c);
}

int fw_getc(void)
{
    static volatile uint8_t in;
    if (!tty_base || !r32(tty_base + TTY_BYTES_READY))
        return -1;
    w32(tty_base + TTY_DATA_PTR, (uint32_t)&in);
    w32(tty_base + TTY_DATA_LEN, 1);
    w32(tty_base + TTY_CMD, TTY_CMD_READ_BUFFER);
    return in;
}

/* Goldfish RTC: nanoseconds since the epoch (reading the low word
 * latches the high one) */
static uint64_t rtc_ns(void)
{
    if (!rtc_base)
        return 0;
    uint32_t lo = r32(rtc_base + 0x00);
    uint32_t hi = r32(rtc_base + 0x04);
    return (uint64_t)hi << 32 | lo;
}

/* virt ctrl */
#define CTRL_CMD            0x04
#define CTRL_CMD_RESET      1
#define CTRL_CMD_HALT       2

static void ctrl(uint32_t cmd)
{
    if (ctrl_base)
        w32(ctrl_base + CTRL_CMD, cmd);
}

/* --- exceptions -------------------------------------------------------- */

struct frame {
    uint32_t d[8], a[7];
    uint16_t sr;
    uint16_t pc_hi, pc_lo;
    uint16_t fmt_vec;
} __attribute__((packed));

void fw_exception(struct frame *f) __attribute__((noreturn));
void fw_exception(struct frame *f)
{
    uint32_t pc = (uint32_t)f->pc_hi << 16 | f->pc_lo;
    fw_printf("trap: vector %u at pc 0x%08x sr 0x%04x, halting\n", (f->fmt_vec & 0xfff) >> 2,
              pc, f->sr);
    for (;;)
        __asm__ volatile("stop #0x2700");
}

/* --- OS image: ELF32 big-endian m68k ----------------------------------- */

#define EM_68K 4

struct os_image {
    uint32_t entry, low, high;
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static const char *os_probe(const uint8_t *f, uint32_t size, struct os_image *img)
{
    if (size < 52 || memcmp(f, "\177ELF", 4))
        return "not an ELF file";
    if (f[4] != 1 || f[5] != 2 || be16(f + 16) != 2)
        return "not a 32-bit big-endian ELF executable";
    if (be16(f + 18) != EM_68K)
        return "not for m68k";
    uint32_t phoff = rd32(f + 28);
    uint16_t phentsize = be16(f + 42), phnum = be16(f + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > size)
        return "program headers run past the file";
    img->entry = rd32(f + 24);
    img->low = ~0u;
    img->high = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + (uint32_t)i * phentsize;
        if (rd32(ph) != 1)          /* PT_LOAD */
            continue;
        uint32_t off = rd32(ph + 4), pa = rd32(ph + 12), fsz = rd32(ph + 16),
                 msz = rd32(ph + 20);
        if (off + fsz > size || fsz > msz)
            return "a segment runs past the file";
        if (pa < img->low)
            img->low = pa;
        if (pa + msz > img->high)
            img->high = pa + msz;
    }
    if (img->high == 0)
        return "no PT_LOAD segment";
    return 0;
}

static void os_place(const uint8_t *f)
{
    uint32_t phoff = rd32(f + 28);
    uint16_t phentsize = be16(f + 42), phnum = be16(f + 44);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + (uint32_t)i * phentsize;
        if (rd32(ph) != 1)
            continue;
        uint32_t off = rd32(ph + 4), pa = rd32(ph + 12), fsz = rd32(ph + 16),
                 msz = rd32(ph + 20);
        memmove((void *)pa, f + off, fsz);
        memset((void *)(pa + fsz), 0, msz - fsz);
    }
}

static int overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

/* The OS's bootinfo: QEMU's records without the ramdisk (that was the OS
 * image itself) and with our command line, then BI_LAST */
static uint32_t os_bootinfo(uint8_t *out, uint32_t max)
{
    uint32_t o = 0, off = 0;
    while (off + 4 <= bootinfo_len) {
        uint16_t tag = be16(bootinfo + off), size = be16(bootinfo + off + 2);
        if (tag == BI_LAST)
            break;
        if (tag != BI_RAMDISK && tag != BI_COMMAND_LINE && o + size + 4 <= max) {
            memcpy(out + o, bootinfo + off, size);
            o += size;
        }
        off += size;
    }
    uint32_t cl = (uint32_t)strlen(cmdline) + 1;
    uint32_t rec = (4 + cl + 3) & ~3u;
    if (o + rec + 4 <= max) {
        memset(out + o, 0, rec);
        out[o] = BI_COMMAND_LINE >> 8;
        out[o + 1] = BI_COMMAND_LINE & 0xff;
        out[o + 2] = (uint8_t)(rec >> 8);
        out[o + 3] = (uint8_t)rec;
        memcpy(out + o + 4, cmdline, cl);
        o += rec;
    }
    memset(out + o, 0, 4);          /* BI_LAST, size 4 */
    out[o + 3] = 4;
    return o + 4;
}

static void boot(void)
{
    if (!rd_size) {
        fw_puts("boot: no OS image (give one with -initrd)\n");
        return;
    }
    const uint8_t *f = (const uint8_t *)rd_addr;
    struct os_image img;
    const char *err = os_probe(f, rd_size, &img);
    if (err) {
        fw_printf("boot: OS image at 0x%x: %s\n", rd_addr, err);
        return;
    }
    uint32_t fw_lo = (uint32_t)_start, fw_len = (uint32_t)_end - fw_lo + 0x1000;
    uint32_t bi_at = (img.high + 1) & ~1u;     /* where QEMU puts it, as Linux expects */
    if (bi_at + 4096 > ram_base + ram_size ||
        overlap(img.low, bi_at + 4096 - img.low, fw_lo, fw_len) ||
        overlap(img.low, bi_at + 4096 - img.low, rd_addr, rd_size)) {
        fw_printf("boot: OS image 0x%x-0x%x overlaps the firmware or its own file, or is out of RAM\n",
                  img.low, img.high);
        return;
    }
    os_place(f);
    uint32_t n = os_bootinfo((uint8_t *)bi_at, 4096);
    fw_printf("boot: ELF at 0x%x-0x%x, entry 0x%x, bootinfo at 0x%x (%u bytes)\n", img.low,
              img.high, img.entry, bi_at, n);
    fw_puts("jumping to the OS\n");
    /* The OS sets its own stack; give it one just below the firmware */
    fw_enter(img.entry, fw_lo);
}

/* --- prompt ------------------------------------------------------------ */

static void print_bi(uint16_t tag, const uint8_t *d, uint32_t len)
{
    static const char *const names[] = { "last", "machtype", "cputype", "fputype", "mmutype",
                                         "memchunk", "ramdisk", "command-line", "rng-seed" };
    static const char *const vnames[] = { "qemu-version", "gf-pic", "gf-rtc", "gf-tty",
                                          "virtio", "virt-ctrl" };
    const char *n = tag < 9 ? names[tag] : (tag >= 0x8000 && tag <= 0x8005) ?
                    vnames[tag - 0x8000] : "?";
    fw_printf("  0x%04x %-13s", tag, n);
    if (tag == BI_COMMAND_LINE)
        fw_printf(" \"%s\"", (const char *)d);
    else
        for (uint32_t i = 0; i + 4 <= len && i < 16; i += 4)
            fw_printf(" 0x%08x", rd32(d + i));
    fw_putc('\n');
}

static void cmd_info(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_printf("SquirrelBIOS %s (m68k-virt)\n", SQ_VERSION);
    fw_printf("ram: 0x%x, %u MB; firmware at 0x%x-0x%x\n", ram_base, ram_size >> 20,
              (uint32_t)_start, (uint32_t)_end);
    fw_printf("qemu: %u.%u.%u, machine type %u\n", qemu_version >> 24,
              (qemu_version >> 16) & 0xff, (qemu_version >> 8) & 0xff, machtype);
    fw_printf("tty 0x%x rtc 0x%x pic 0x%x ctrl 0x%x virtio 0x%x\n", tty_base, rtc_base,
              pic_base, ctrl_base, virtio_base);
    if (rd_size)
        fw_printf("os image: 0x%x, %u bytes\n", rd_addr, rd_size);
}

static void cmd_bootinfo(int argc, char **argv)
{
    (void)argc, (void)argv;
    bi_walk(bootinfo, bootinfo_len, print_bi);
}

static void cmd_date(int argc, char **argv)
{
    (void)argc, (void)argv;
    uint64_t s = rtc_ns() / 1000000000ull;
    /* civil date from days since 1970-01-01 */
    uint32_t days = (uint32_t)(s / 86400), rem = (uint32_t)(s % 86400);
    int32_t z = (int32_t)days + 719468, era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9, y = yoe + (uint32_t)era * 400 + (m <= 2);
    fw_printf("%04u-%02u-%02u %02u:%02u:%02u UTC\n", y, m, d, rem / 3600, rem / 60 % 60,
              rem % 60);
}

static void cmd_boot(int argc, char **argv)
{
    (void)argc, (void)argv;
    boot();
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_puts("powering off\n");
    ctrl(CTRL_CMD_HALT);
}

static void cmd_reset(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_puts("resetting\n");
    ctrl(CTRL_CMD_RESET);
}

static const struct fw_cmd cmds[] = {
    { "info", "firmware, memory and devices", cmd_info },
    { "bootinfo", "the machine's bootinfo records", cmd_bootinfo },
    { "date", "the RTC", cmd_date },
    { "boot", "boot the OS image (-initrd) again", cmd_boot },
    { "poweroff", "power off", cmd_poweroff },
    { "reset", "reset the machine", cmd_reset },
    { 0, 0, 0 },
};

/* --- main ---------------------------------------------------------------- */

void fw_main(void)
{
    /* QEMU's records follow the image, at the next even address */
    const uint8_t *bi = (const uint8_t *)(((uint32_t)_end + 1) & ~1u);
    bootinfo_len = bi_walk(bi, sizeof(bootinfo), 0);
    memcpy(bootinfo, bi, bootinfo_len);
    bi_walk(bootinfo, bootinfo_len, bi_take);

    fw_printf("\nSquirrelBIOS m68k %s (m68k-virt)\n", SQ_VERSION);
    if (!bootinfo_len || machtype != MACH_VIRT) {
        fw_printf("bootinfo: %s, halting\n", bootinfo_len ? "not a virt machine" : "none found");
        for (;;)
            __asm__ volatile("stop #0x2700");
    }
    fw_printf("mem: %u MB at 0x%x, firmware at 0x%x\n", ram_size >> 20, ram_base,
              (uint32_t)_start);
    if (cmdline[0])
        fw_printf("bootargs: %s\n", cmdline);
    boot();
    fw_puts("type help for commands\n");
    fw_prompt(cmds);
}
