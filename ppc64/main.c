/* SquirrelBIOS ppc64le-powernv: firmware for QEMU's powernv machines
 * (POWER9 and POWER10). Console, CPU parking, the OPAL-style runtime
 * interface, the device tree hand-off and the kernel load.
 * Original code.
 */
#include "fwlib.h"
#include "opal.h"

#ifdef SQ_CONFIG
#include "config.h"
#endif
#ifndef CONFIG_MAX_CPUS
#define CONFIG_MAX_CPUS 64
#endif
#ifndef CONFIG_KERNEL_BASE
#define CONFIG_KERNEL_BASE 0x20000000
#endif
#ifndef CONFIG_KERNEL_MAX
#define CONFIG_KERNEL_MAX 0x08000000
#endif

#define SQ_VERSION   "3.0.0-alpha"
#define EM_PPC64     21
#define FDT_OUT_SIZE (256 * 1024)
#define CPU_STACK    (16 * 1024)

extern char _start[], __fw_end[];
extern char opal_entry[];
void fw_enter(uint64_t entry, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r8,
              uint64_t r9) __attribute__((noreturn));
void fw_park(uint64_t stack_top, uint32_t pir) __attribute__((noreturn));

#define FW_BASE ((uint64_t)(uintptr_t)_start)
/* The firmware's region: image, .bss (stacks, tree) and some headroom */
#define FW_SIZE ((uint64_t)0x400000)

/* --- low-level ------------------------------------------------------- */

static inline uint8_t in8(uint64_t a)
{
    uint8_t v;
    __asm__ volatile("lbzcix %0,0,%1" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline void out8(uint64_t a, uint8_t v)
{
    __asm__ volatile("stbcix %0,0,%1" : : "r"(v), "r"(a) : "memory");
}

static inline uint64_t mftb(void)
{
    uint64_t v;
    __asm__ volatile("mftb %0" : "=r"(v));
    return v;
}

static inline uint32_t mfpir(void)
{
    uint64_t v;
    __asm__ volatile("mfspr %0,1023" : "=r"(v));
    return (uint32_t)v;
}

static inline void sync(void)
{
    __asm__ volatile("sync" : : : "memory");
}

static inline void relax(void)
{
    __asm__ volatile("or 1,1,1; or 2,2,2" : : : "memory");   /* low, then medium priority */
}

static uint64_t tb_freq = 512000000;

static void udelay(uint64_t us)
{
    uint64_t end = mftb() + us * (tb_freq / 1000000);
    while ((int64_t)(mftb() - end) < 0)
        relax();
}

/* --- console: the 16550 on the LPC bus ------------------------------- */

static uint64_t uart;           /* CPU address of the UART, 0 = none */

#define UART_THR 0
#define UART_RBR 0
#define UART_IER 1
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_DLL 0
#define UART_DLM 1
#define LSR_DR   0x01
#define LSR_THRE 0x20

static void uart_init(uint32_t clock)
{
    uint32_t div = clock ? clock / (16 * 115200) : 1;
    if (!div)
        div = 1;
    out8(uart + UART_IER, 0);
    out8(uart + UART_LCR, 0x80);        /* DLAB */
    out8(uart + UART_DLL, div & 0xff);
    out8(uart + UART_DLM, div >> 8);
    out8(uart + UART_LCR, 0x03);        /* 8N1 */
    /* FIFOs stay as they are: turning them on would drop typed-ahead
     * input, and a polled console does not need them */
    out8(uart + UART_MCR, 0x03);        /* DTR, RTS */
}

void fw_putc(char c)
{
    if (!uart)
        return;
    for (int i = 0; i < 100000 && !(in8(uart + UART_LSR) & LSR_THRE); i++)
        ;
    out8(uart + UART_THR, (uint8_t)c);
}

int fw_getc(void)
{
    if (!uart || !(in8(uart + UART_LSR) & LSR_DR))
        return -1;
    return in8(uart + UART_RBR);
}

/* --- RTC: MC146818 on the LPC bus ------------------------------------ */

static uint64_t rtc;

static uint8_t cmos_read(uint8_t reg)
{
    out8(rtc, reg);
    return in8(rtc + 1);
}

static void cmos_write(uint8_t reg, uint8_t v)
{
    out8(rtc, reg);
    out8(rtc + 1, v);
}

static uint8_t to_bcd(uint8_t v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

/* Reads the clock as OPAL returns it: 0xYYYYMMDD and 0xHHMMSS00... in BCD */
static int rtc_read(uint32_t *ymd, uint64_t *hms)
{
    if (!rtc)
        return -1;
    for (int i = 0; i < 10000 && (cmos_read(0x0a) & 0x80); i++)  /* update in progress */
        udelay(10);
    uint8_t sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32);
    if (cmos_read(0x0b) & 0x04) {          /* binary mode: convert */
        sec = to_bcd(sec), min = to_bcd(min), hour = to_bcd(hour);
        day = to_bcd(day), mon = to_bcd(mon), year = to_bcd(year), cent = to_bcd(cent);
    }
    if (cent == 0)
        cent = 0x20;
    *ymd = (uint32_t)cent << 24 | (uint32_t)year << 16 | (uint32_t)mon << 8 | day;
    *hms = (uint64_t)hour << 56 | (uint64_t)min << 48 | (uint64_t)sec << 40;
    return 0;
}

static int rtc_write(uint32_t ymd, uint64_t hms)
{
    if (!rtc)
        return -1;
    uint8_t b = cmos_read(0x0b);
    cmos_write(0x0b, b | 0x80);             /* hold updates */
    cmos_write(0x00, (hms >> 40) & 0xff);
    cmos_write(0x02, (hms >> 48) & 0xff);
    cmos_write(0x04, (hms >> 56) & 0xff);
    cmos_write(0x07, ymd & 0xff);
    cmos_write(0x08, (ymd >> 8) & 0xff);
    cmos_write(0x09, (ymd >> 16) & 0xff);
    cmos_write(0x32, (ymd >> 24) & 0xff);
    cmos_write(0x0b, (uint8_t)(b & ~0x84));  /* BCD, running */
    return 0;
}

/* --- IPMI over the BT interface: chassis power ----------------------- */

static uint64_t bt;
#define BT_CTRL         0
#define BT_BUF          1
#define BT_CLR_WR_PTR   0x01
#define BT_CLR_RD_PTR   0x02
#define BT_H2B_ATN      0x04
#define BT_B2H_ATN      0x08
#define BT_H_BUSY       0x40
#define BT_B_BUSY       0x80

/* Sends one request; returns the completion code, or -1 */
static int ipmi_cmd(uint8_t netfn, uint8_t cmd, const uint8_t *data, int len)
{
    static uint8_t seq;
    if (!bt)
        return -1;
    int i;
    for (i = 0; i < 100000 && (in8(bt + BT_CTRL) & (BT_B_BUSY | BT_H2B_ATN)); i++)
        udelay(10);
    out8(bt + BT_CTRL, BT_CLR_WR_PTR);
    out8(bt + BT_BUF, (uint8_t)(len + 3));
    out8(bt + BT_BUF, (uint8_t)(netfn << 2));
    out8(bt + BT_BUF, ++seq);
    out8(bt + BT_BUF, cmd);
    for (i = 0; i < len; i++)
        out8(bt + BT_BUF, data[i]);
    out8(bt + BT_CTRL, BT_H2B_ATN);
    for (i = 0; i < 100000 && !(in8(bt + BT_CTRL) & BT_B2H_ATN); i++)
        udelay(10);
    if (!(in8(bt + BT_CTRL) & BT_B2H_ATN))
        return -1;
    out8(bt + BT_CTRL, BT_H_BUSY);
    out8(bt + BT_CTRL, BT_B2H_ATN);
    out8(bt + BT_CTRL, BT_CLR_RD_PTR);
    int rlen = in8(bt + BT_BUF);
    int cc = -1;
    for (i = 0; i < rlen; i++) {
        uint8_t b = in8(bt + BT_BUF);
        if (i == 3)
            cc = b;     /* netfn, seq, cmd, completion code */
    }
    out8(bt + BT_CTRL, BT_H_BUSY);
    return cc;
}

static int chassis(uint8_t action)  /* 0 power down, 3 hard reset */
{
    return ipmi_cmd(0x00, 0x02, &action, 1);
}

/* --- CPUs -------------------------------------------------------------- */

struct cpu {
    uint32_t pir;
    volatile uint32_t state;        /* OPAL_THREAD_* */
    volatile uint32_t checked_in;
    volatile uint64_t start;
};

static struct cpu cpus[CONFIG_MAX_CPUS];
static int ncpus;
static uint32_t boot_pir;
static uint8_t cpu_stacks[CONFIG_MAX_CPUS][CPU_STACK] __attribute__((aligned(16)));
volatile uint32_t cpus_ready;
uint64_t opal_stack_by_pir[4096];

static struct cpu *cpu_by_pir(uint32_t pir)
{
    for (int i = 0; i < ncpus; i++)
        if (cpus[i].pir == pir)
            return &cpus[i];
    return 0;
}

static uint64_t stack_top(struct cpu *c)
{
    return (uint64_t)(uintptr_t)cpu_stacks[c - cpus] + CPU_STACK;
}

uint64_t cpu_stack_for(uint32_t pir)
{
    struct cpu *c = cpu_by_pir(pir);
    return c ? stack_top(c) : 0;
}

void fw_secondary(uint32_t pir) __attribute__((noreturn));
void fw_secondary(uint32_t pir)
{
    struct cpu *c = cpu_by_pir(pir);
    c->state = OPAL_THREAD_INACTIVE;
    c->checked_in = 1;
    sync();
    while (c->state != OPAL_THREAD_STARTED)
        relax();
    sync();
    __asm__ volatile("or 2,2,2");       /* back to medium priority */
    fw_enter(c->start, pir, 0, 0, 0, 0);
}

static int cpus_from_dt(const void *fdt)
{
    for (int n = dtb_find_device_type(fdt, -1, "cpu"); n >= 0;
         n = dtb_find_device_type(fdt, n, "cpu")) {
        int len;
        const uint8_t *s = dtb_prop(fdt, n, "ibm,ppc-interrupt-server#s", &len);
        if (!s) {
            s = dtb_prop(fdt, n, "reg", &len);
            if (!s)
                continue;
            len = 4;
        }
        if (tb_freq == 512000000)
            tb_freq = dtb_prop_u32(fdt, n, "timebase-frequency", 512000000);
        for (int i = 0; i + 4 <= len; i += 4) {
            if (ncpus == CONFIG_MAX_CPUS) {
                fw_printf("cpu: more than %d threads, the rest stay parked\n", CONFIG_MAX_CPUS);
                return ncpus;
            }
            struct cpu *c = &cpus[ncpus++];
            c->pir = fw_get_be32(s + i);
            c->state = OPAL_THREAD_UNAVAILABLE;
            opal_stack_by_pir[c->pir & 0xfff] = stack_top(c);
        }
    }
    return ncpus;
}

/* --- OPAL-style runtime ------------------------------------------------ */

static const uint16_t opal_tokens[] = {
    OPAL_TEST, OPAL_CONSOLE_WRITE, OPAL_CONSOLE_READ, OPAL_RTC_READ, OPAL_RTC_WRITE,
    OPAL_CEC_POWER_DOWN, OPAL_CEC_REBOOT, OPAL_POLL_EVENTS,
    OPAL_CONSOLE_WRITE_BUFFER_SPACE, OPAL_START_CPU, OPAL_QUERY_CPU_STATUS,
    OPAL_RETURN_CPU, OPAL_REINIT_CPUS, OPAL_CHECK_TOKEN, OPAL_CONSOLE_FLUSH,
};

static int token_present(uint64_t t)
{
    for (unsigned i = 0; i < sizeof(opal_tokens) / sizeof(opal_tokens[0]); i++)
        if (opal_tokens[i] == t)
            return 1;
    return 0;
}

#define PTR(x) ((void *)(uintptr_t)(x))

/* r[0] = token, r[1..8] = r3..r10 as the caller passed them */
int64_t opal_dispatch(uint64_t *r)
{
    uint64_t token = r[0], a0 = r[1], a1 = r[2], a2 = r[3];
    switch (token) {
    case OPAL_TEST:
        return OPAL_SUCCESS;
    case OPAL_CONSOLE_WRITE: {
        int64_t *len = PTR(a1);
        const uint8_t *buf = PTR(a2);
        if (a0 != 0 || !len)
            return OPAL_PARAMETER;
        for (int64_t i = 0; i < *len; i++)
            fw_putc((char)buf[i]);
        return OPAL_SUCCESS;
    }
    case OPAL_CONSOLE_READ: {
        int64_t *len = PTR(a1);
        uint8_t *buf = PTR(a2);
        if (a0 != 0 || !len)
            return OPAL_PARAMETER;
        int64_t n = 0;
        for (int c; n < *len && (c = fw_getc()) >= 0; n++)
            buf[n] = (uint8_t)c;
        *len = n;
        return OPAL_SUCCESS;
    }
    case OPAL_CONSOLE_WRITE_BUFFER_SPACE: {
        int64_t *len = PTR(a1);
        if (a0 != 0 || !len)
            return OPAL_PARAMETER;
        *len = 4096;
        return OPAL_SUCCESS;
    }
    case OPAL_CONSOLE_FLUSH:
        return a0 == 0 ? OPAL_SUCCESS : OPAL_PARAMETER;
    case OPAL_RTC_READ: {
        uint32_t *ymd = PTR(a0);
        uint64_t *hms = PTR(a1);
        if (!ymd || !hms)
            return OPAL_PARAMETER;
        return rtc_read(ymd, hms) ? OPAL_HARDWARE : OPAL_SUCCESS;
    }
    case OPAL_RTC_WRITE:
        return rtc_write((uint32_t)a0, a1) ? OPAL_HARDWARE : OPAL_SUCCESS;
    case OPAL_CEC_POWER_DOWN:
        fw_puts("opal: power down\n");
        return chassis(0) == 0 ? OPAL_SUCCESS : OPAL_UNSUPPORTED;
    case OPAL_CEC_REBOOT:
        fw_puts("opal: reboot\n");
        return chassis(3) == 0 ? OPAL_SUCCESS : OPAL_UNSUPPORTED;
    case OPAL_POLL_EVENTS:
        if (a0)
            *(uint64_t *)PTR(a0) = 0;
        return OPAL_SUCCESS;
    case OPAL_START_CPU: {
        struct cpu *c = cpu_by_pir((uint32_t)a0);
        if (!c)
            return OPAL_PARAMETER;
        if (c->state != OPAL_THREAD_INACTIVE)
            return OPAL_WRONG_STATE;
        c->start = a1;
        sync();
        c->state = OPAL_THREAD_STARTED;
        sync();
        return OPAL_SUCCESS;
    }
    case OPAL_QUERY_CPU_STATUS: {
        struct cpu *c = cpu_by_pir((uint32_t)a0);
        uint8_t *st = PTR(a1);
        if (!c || !st)
            return OPAL_PARAMETER;
        *st = (uint8_t)c->state;
        return OPAL_SUCCESS;
    }
    case OPAL_RETURN_CPU: {
        struct cpu *c = cpu_by_pir(mfpir());
        if (!c)
            return OPAL_INTERNAL_ERROR;
        fw_park(stack_top(c), c->pir);
    }
    case OPAL_REINIT_CPUS:
        return OPAL_SUCCESS;
    case OPAL_CHECK_TOKEN:
        return token_present(a0) ? OPAL_TOKEN_PRESENT : OPAL_TOKEN_ABSENT;
    }
    return OPAL_PARAMETER;      /* what OPAL returns for an unknown token */
}

/* --- exceptions before the OS ----------------------------------------- */

void fw_trap(uint64_t vec, uint64_t r3, uint64_t srr0, uint64_t srr1, uint64_t hsrr0,
             uint64_t hsrr1)
{
    (void)r3;
    fw_printf("trap: vector 0x%lx srr0 0x%lx srr1 0x%lx hsrr0 0x%lx hsrr1 0x%lx, halting\n",
              vec, srr0, srr1, hsrr0, hsrr1);
    for (;;)
        relax();
}

/* --- device tree for the OS -------------------------------------------- */

static const void *fdt_in;
static uint8_t fdt_out[FDT_OUT_SIZE] __attribute__((aligned(8)));
static uint64_t ram_size;

struct dt_ctx {
    int saw_opal;
};

static void opal_props(struct fdt_writer *w)
{
    fdtw_prop_str2(w, "compatible", "ibm,opal-v3", "ibm,opal-v2");
    fdtw_prop_u64(w, "opal-base-address", FW_BASE);
    fdtw_prop_u64(w, "opal-entry-address", (uint64_t)(uintptr_t)opal_entry);
    fdtw_prop_u64(w, "opal-runtime-size", FW_SIZE);
    fdtw_prop_str(w, "squirrel,firmware", "SquirrelBIOS " SQ_VERSION);
    fdtw_begin(w, "consoles");
    fdtw_prop_u32(w, "#address-cells", 1);
    fdtw_prop_u32(w, "#size-cells", 0);
    fdtw_begin(w, "serial@0");
    fdtw_prop_str(w, "compatible", "ibm,opal-console-raw");
    fdtw_prop_str(w, "device_type", "serial");
    fdtw_prop_u32(w, "reg", 0);
    fdtw_prop_u32(w, "#write-buffer-size", 4096);
    fdtw_end(w);
    fdtw_end(w);
}

static int is_root_child(const void *fdt, int node, const char *name)
{
    return !strcmp(dtb_name(fdt, node), name) && dtb_parent(fdt, node) == dtb_root(fdt);
}

static int dt_skip(const void *src, int node, const char *prop, void *vctx)
{
    (void)vctx;
    if (is_root_child(src, node, "ibm,opal"))
        return !strcmp(prop, "compatible") || !strncmp(prop, "opal-", 5);
    if (is_root_child(src, node, "chosen"))
        return !strcmp(prop, "linux,stdout-path") || !strcmp(prop, "stdout-path");
    return 0;
}

static void dt_hook(struct fdt_writer *w, const void *src, int node, void *vctx)
{
    struct dt_ctx *ctx = vctx;
    if (is_root_child(src, node, "ibm,opal")) {
        ctx->saw_opal = 1;
        opal_props(w);
    } else if (is_root_child(src, node, "chosen")) {
        fdtw_prop_str(w, "linux,stdout-path", "/ibm,opal/consoles/serial@0");
        fdtw_prop_str(w, "stdout-path", "/ibm,opal/consoles/serial@0");
    } else if (node == dtb_root(src)) {
        /* The root's own properties are done; add ibm,opal if the
         * source tree has none (its children come after this) */
        int has = 0;
        for (int c = dtb_first_child(src, node); c >= 0; c = dtb_next_sibling(src, c))
            if (!strcmp(dtb_name(src, c), "ibm,opal"))
                has = 1;
        if (!has) {
            ctx->saw_opal = 1;
            fdtw_begin(w, "ibm,opal");
            opal_props(w);
            fdtw_end(w);
        }
    }
}

static void *build_dt(void)
{
    struct fdt_writer w;
    struct dt_ctx ctx = { 0 };
    fdtw_init(&w, fdt_out, sizeof(fdt_out));
    /* Keep QEMU's reservations, and add the firmware's own region */
    const uint8_t *rsv = (const uint8_t *)fdt_in + fw_get_be32((const uint8_t *)fdt_in + 16);
    for (; fw_get_be64(rsv + 8); rsv += 16)
        fdtw_reserve(&w, fw_get_be64(rsv), fw_get_be64(rsv + 8));
    fdtw_reserve(&w, FW_BASE, FW_SIZE);
    fdtw_copy(&w, fdt_in, dtb_root(fdt_in), dt_hook, dt_skip, &ctx);
    uint32_t size = fdtw_finish(&w, boot_pir);
    if (!size) {
        fw_puts("dt: device tree does not fit in 256 KB\n");
        return 0;
    }
    return fdt_out;
}

/* --- boot ---------------------------------------------------------------- */

static int ranges_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

static void boot_kernel(void)
{
    const void *file = PTR(CONFIG_KERNEL_BASE);
    struct elf_image img;
    int err = elf_probe(file, CONFIG_KERNEL_MAX, EM_PPC64, &img);
    if (err) {
        if (err == -1 && fw_get_be32(file) == 0)
            fw_printf("boot: no kernel at 0x%x\n", CONFIG_KERNEL_BASE);
        else
            fw_printf("boot: kernel at 0x%x: %s\n", CONFIG_KERNEL_BASE, elf_error(err));
        return;
    }
    if (img.high > ram_size || img.low < 0x4000 ||
        ranges_overlap(img.low, img.high - img.low, FW_BASE, FW_SIZE) ||
        (!img.in_place && ranges_overlap(img.low, img.high - img.low, CONFIG_KERNEL_BASE,
                                         CONFIG_KERNEL_MAX))) {
        fw_printf("boot: kernel at 0x%lx-0x%lx overlaps the firmware or is out of RAM\n",
                  img.low, img.high);
        return;
    }
    elf_place(file, &img);
    void *dt = build_dt();
    if (!dt)
        return;
    fw_printf("boot: %s-endian ELF kernel, %s at 0x%lx, entry 0x%lx\n",
              img.big_endian ? "big" : "little", img.in_place ? "in place" : "loaded",
              img.low, img.entry_phys);
    fw_printf("boot: device tree at 0x%lx, OPAL at 0x%lx entry 0x%lx\n",
              (uint64_t)(uintptr_t)dt, FW_BASE, (uint64_t)(uintptr_t)opal_entry);
    fw_puts("jumping to kernel\n");
    /* Instructions written by elf_place must reach instruction fetch */
    for (uint64_t a = img.low & ~127ull; a < img.high; a += 128)
        __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    sync();
    for (uint64_t a = img.low & ~127ull; a < img.high; a += 128)
        __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync; isync" : : : "memory");
    fw_enter(img.entry_phys, (uint64_t)(uintptr_t)dt, img.in_place ? img.low : 0, 0, FW_BASE,
             (uint64_t)(uintptr_t)opal_entry);
}

/* --- prompt -------------------------------------------------------------- */

static void cmd_info(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_printf("SquirrelBIOS %s (ppc64le-powernv)\n", SQ_VERSION);
    fw_printf("model: %s\n", (const char *)dtb_prop(fdt_in, dtb_root(fdt_in), "model", 0));
    fw_printf("ram: %lu MB, firmware at 0x%lx (%lu KB)\n", ram_size >> 20, FW_BASE, FW_SIZE >> 10);
    fw_printf("cpus: %d threads, boot PIR 0x%x\n", ncpus, boot_pir);
    for (int i = 0; i < ncpus; i++)
        fw_printf("  PIR 0x%x %s\n", cpus[i].pir,
                  cpus[i].pir == boot_pir ? "boot" :
                  cpus[i].state == OPAL_THREAD_INACTIVE ? "parked" : "unavailable");
}

static void cmd_dt(int argc, char **argv)
{
    int node = dtb_path(fdt_in, argc > 1 ? argv[1] : "/");
    if (node < 0)
        fw_printf("dt: no node %s\n", argv[1]);
    else
        dtb_print(fdt_in, node);
}

static void cmd_date(int argc, char **argv)
{
    (void)argc, (void)argv;
    uint32_t ymd;
    uint64_t hms;
    if (rtc_read(&ymd, &hms))
        fw_puts("date: no RTC\n");
    else
        fw_printf("%04x-%02x-%02x %02x:%02x:%02x UTC\n", ymd >> 16, (ymd >> 8) & 0xff,
                  ymd & 0xff, (unsigned)(hms >> 56), (unsigned)(hms >> 48) & 0xff,
                  (unsigned)(hms >> 40) & 0xff);
}

static void cmd_boot(int argc, char **argv)
{
    (void)argc, (void)argv;
    boot_kernel();
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_puts("powering off\n");
    if (chassis(0))
        fw_puts("poweroff: no BMC answered\n");
}

static void cmd_reset(int argc, char **argv)
{
    (void)argc, (void)argv;
    fw_puts("resetting\n");
    if (chassis(3))
        fw_puts("reset: no BMC answered\n");
}

static const struct fw_cmd commands[] = {
    { "info", "firmware, memory and CPUs", cmd_info },
    { "dt", "print the device tree (or one node: dt /cpus)", cmd_dt },
    { "date", "read the real-time clock", cmd_date },
    { "boot", "boot the -kernel image again", cmd_boot },
    { "poweroff", "power off through the BMC", cmd_poweroff },
    { "reset", "reset through the BMC", cmd_reset },
    { 0, 0, 0 },
};

/* --- main ------------------------------------------------------------------ */

static uint64_t node_addr(const void *fdt, const char *compat)
{
    uint64_t a;
    int n = dtb_find_compatible(fdt, -1, compat);
    if (n < 0 || dtb_reg(fdt, n, 0, &a, 0))
        return 0;
    return a;
}

void fw_main(const void *fdt, uint32_t pir)
{
    fdt_in = fdt;
    boot_pir = pir;
    if (dtb_check(fdt))
        for (;;)
            relax();        /* no tree: no console either */

    int serial = dtb_find_compatible(fdt, -1, "ns16550");
    if (serial >= 0 && !dtb_reg(fdt, serial, 0, &uart, 0))
        uart_init(dtb_prop_u32(fdt, serial, "clock-frequency", 1843200));
    fw_printf("\nSquirrelBIOS POWER %s (ppc64le-powernv)\n", SQ_VERSION);
    fw_printf("model: %s\n", (const char *)dtb_prop(fdt, dtb_root(fdt), "model", 0));

    /* RAM */
    for (int n = dtb_find_device_type(fdt, -1, "memory"); n >= 0;
         n = dtb_find_device_type(fdt, n, "memory")) {
        uint64_t base, size;
        for (int i = 0; !dtb_reg(fdt, n, i, &base, &size); i++) {
            fw_printf("mem: 0x%016lx-0x%016lx\n", base, base + size - 1);
            if (base + size > ram_size)
                ram_size = base + size;
        }
    }
    if (ram_size < FW_BASE + FW_SIZE) {
        fw_printf("mem: firmware needs RAM up to 0x%lx, boot halted\n", FW_BASE + FW_SIZE);
        for (;;)
            relax();
    }
    if ((uint64_t)(uintptr_t)__fw_end > FW_BASE + FW_SIZE) {
        fw_puts("fw: image larger than its region, boot halted\n");
        for (;;)
            relax();
    }

    /* CPUs: number them, release the waiting threads into their slots */
    cpus_from_dt(fdt);
    struct cpu *me = cpu_by_pir(pir);
    if (me)
        me->state = OPAL_THREAD_STARTED;
    sync();
    cpus_ready = 1;
    sync();
    int parked = 0;
    for (uint64_t end = mftb() + tb_freq / 2; (int64_t)(mftb() - end) < 0;) {
        parked = 0;
        for (int i = 0; i < ncpus; i++)
            parked += cpus[i].checked_in;
        if (parked == ncpus - 1)
            break;
        relax();
    }
    fw_printf("cpu: %d threads, boot PIR 0x%x, %d parked\n", ncpus, pir, parked);
    if (parked != ncpus - 1)
        fw_printf("cpu: %d threads did not check in\n", ncpus - 1 - parked);

    rtc = node_addr(fdt, "pnpPNP,b00");
    bt = node_addr(fdt, "ipmi-bt");
    fw_printf("opal: entry 0x%lx, %d calls, rtc %s, bmc %s\n", (uint64_t)(uintptr_t)opal_entry,
              (int)(sizeof(opal_tokens) / sizeof(opal_tokens[0])), rtc ? "yes" : "no",
              bt ? "yes" : "no");

    boot_kernel();
    fw_puts("type help for commands\n");
    fw_prompt(commands);
}
