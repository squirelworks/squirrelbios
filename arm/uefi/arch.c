/* SquirrelBIOS aarch64-uefi: the AArch64 / QEMU virt side of the UEFI
 * core. Original code, written from the Arm Architecture Reference
 * Manual (A-profile), the PL011 and PL031 technical reference manuals,
 * the GICv3 architecture specification and the Arm Power State
 * Coordination Interface (PSCI, DEN0022).
 *
 * Console: PL011. Time: the generic timer's virtual counter; the PL031
 * RTC. Reset and power-off: PSCI through the conduit the device tree
 * names. Memory: an identity map with 1 GB blocks (Device below 1 GB and
 * from 256 GB, Normal write-back for RAM), caches on, at EL1 or EL2. */
#include "efi.h"
#include "../../common/fw/fdt.h"

extern char __fw_start[], __fw_end[];

static uptr uart = CONFIG_UART_BASE;
static uptr rtc;
static uptr gicd, gicr;
static u32 gic_version;
static int psci_smc = -1;               /* -1: no PSCI, 0: hvc, 1: smc */
static u32 el;
extern int el3_active;                  /* el3.c: started at EL3, the monitor runs */
void el3_report(void);

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2C
#define PL011_CR   0x30
#define PL011_IMSC 0x38
#define FR_RXFE    0x10
#define FR_TXFF    0x20

#define read_sysreg(r) ({ u64 _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((u64)(v)))

/* ------------------------------------------------------------ console */

/* A PL011 a boot loader already enabled (the Pi's firmware, with its own
 * clock and baud rate) is used as it is */
void arch_con_init(void)
{
    if (mmio_read32(uart + PL011_CR) & 1)
        return;
    mmio_write32(uart + PL011_CR, 0);
    mmio_write32(uart + PL011_IMSC, 0);
    mmio_write32(uart + PL011_IBRD, 13);        /* 115200 from a 24 MHz clock */
    mmio_write32(uart + PL011_FBRD, 1);
    mmio_write32(uart + PL011_LCRH, 0x70);      /* 8N1, FIFOs on */
    mmio_write32(uart + PL011_CR, 0x301);       /* RX, TX, UART enable */
}

void arch_con_putc(u8 c)
{
    for (int i = 0; i < 1000000 && (mmio_read32(uart + PL011_FR) & FR_TXFF); i++)
        ;
    mmio_write32(uart + PL011_DR, c);
}

int arch_con_getc(void)
{
    if (mmio_read32(uart + PL011_FR) & FR_RXFE)
        return -1;
    return (int)(mmio_read32(uart + PL011_DR) & 0xFF);
}

/* --------------------------------------------------------- CPU, time */

u64 arch_counter(void)
{
    __asm__ volatile("isb");
    return read_sysreg(cntvct_el0);
}

u64 arch_counter_hz(void)
{
    return read_sysreg(cntfrq_el0) & 0xFFFFFFFF;
}

void arch_relax(void)
{
    __asm__ volatile("yield");
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("msr daifset, #0xf; wfi");
}

void arch_sync_icache(void *p, usize n)
{
    u64 ctr = read_sysreg(ctr_el0);
    usize dline = 4UL << ((ctr >> 16) & 0xF), iline = 4UL << (ctr & 0xF);
    uptr a = (uptr)p, end = a + n;
    for (uptr x = a & ~(dline - 1); x < end; x += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(x) : "memory");
    __asm__ volatile("dsb ish");
    for (uptr x = a & ~(iline - 1); x < end; x += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(x) : "memory");
    __asm__ volatile("dsb ish; isb");
}

int arch_rtc_seconds(u64 *secs)
{
    if (!rtc)
        return 0;
    *secs = mmio_read32(rtc);                   /* RTCDR */
    return 1;
}

/* ------------------------------------------------------------- PSCI */

#define PSCI_VERSION      0x84000000
#define PSCI_SYSTEM_OFF   0x84000008
#define PSCI_SYSTEM_RESET 0x84000009

static u64 psci(u64 fn, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    if (psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return x0;
}

/* Without PSCI (the Raspberry Pi's stock boot stub): the BCM2835 power
 * manager's watchdog. A reset is a full reset after a few watchdog ticks;
 * for a shutdown the reset status first names partition 63, which the
 * Pi's boot ROM and firmware take as "halt". */
#define PM_RSTC     0x1C
#define PM_RSTS     0x20
#define PM_WDOG     0x24
#define PM_PASSWORD 0x5A000000u
#define PM_RSTC_WRCFG_FULL 0x20u

static uptr pm_wdog;

static void pm_reset(int shutdown)
{
    if (!pm_wdog)
        return;
    if (shutdown) {
        u32 rsts = mmio_read32(pm_wdog + PM_RSTS) & ~0xFAAu;
        mmio_write32(pm_wdog + PM_RSTS, PM_PASSWORD | rsts | 0x555);    /* partition 63 */
    }
    mmio_write32(pm_wdog + PM_WDOG, PM_PASSWORD | 10);
    u32 rstc = mmio_read32(pm_wdog + PM_RSTC) & ~0x30u;
    mmio_write32(pm_wdog + PM_RSTC, PM_PASSWORD | rstc | PM_RSTC_WRCFG_FULL);
    for (int i = 0; i < 1000000; i++)
        arch_relax();
}

void arch_reset(int type, u64 status)
{
    (void)status;
    if (psci_smc >= 0)
        psci(type == EfiResetShutdown ? PSCI_SYSTEM_OFF : PSCI_SYSTEM_RESET, 0, 0, 0);
    pm_reset(type == EfiResetShutdown);
    if (!efi_boot_services_exited())
        printf("fw: no PSCI or watchdog to %s with, halting\n", type == EfiResetShutdown ? "power off" : "reset");
    arch_halt();
}

/* -------------------------------------------------------------- MMU */

#define PAGE_TABLE_ENTRIES 512
static u64 l0[PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));
static u64 l1[2][PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));

#define DESC_TABLE   0x3ULL
#define DESC_BLOCK   0x1ULL
#define ATTR_DEVICE  (0ULL << 2)        /* MAIR index 0: Device-nGnRnE */
#define ATTR_NORMAL  (1ULL << 2)        /* MAIR index 1: Normal write-back */
#define AP_EL2_RES1  (1ULL << 6)
#define SH_INNER     (3ULL << 8)
#define AF           (1ULL << 10)
#define PXN          (1ULL << 53)
#define XN           (1ULL << 54)

static u32 current_el(void)
{
    return (u32)(read_sysreg(CurrentEL) >> 2) & 3;
}

/* RAM ranges from the device tree's /memory nodes, read with the MMU
 * off before anything else: the map below is made from them */
#define RAM_MAX 16
static u64 ram_base[RAM_MAX], ram_end[RAM_MAX];
static int ram_count;

static void ram_ranges(const void *dt)
{
    ram_count = 0;
    if (!dt || dtb_check(dt))
        return;
    int root = dtb_root(dt);
    int ac = dtb_addr_cells(dt, root), sc = dtb_size_cells(dt, root);
    for (int n = dtb_first_child(dt, root); n >= 0; n = dtb_next_sibling(dt, n)) {
        int len;
        const char *t = dtb_prop(dt, n, "device_type", &len);
        const char *st = dtb_prop(dt, n, "status", &len);
        if (!t || strcmp(t, "memory") || (st && strcmp(st, "okay") && strcmp(st, "ok")))
            continue;
        const u8 *r = dtb_prop(dt, n, "reg", &len);
        for (int o = 0; r && ac && o + 4 * (ac + sc) <= len && ram_count < RAM_MAX; o += 4 * (ac + sc)) {
            u64 b = dtb_cells(r + o, ac), sz = sc ? dtb_cells(r + o + 4 * ac, sc) : 0;
            if (sz) {
                ram_base[ram_count] = b;
                ram_end[ram_count++] = b + sz;
            }
        }
    }
}

/* 1: [a, a + n) is all RAM; 0: none of it is; -1: some of it */
static int ram_cover(u64 a, u64 n)
{
    u64 in = 0;
    for (int i = 0; i < ram_count; i++) {
        u64 lo = ram_base[i] > a ? ram_base[i] : a;
        u64 hi = ram_end[i] < a + n ? ram_end[i] : a + n;
        if (hi > lo)
            in += hi - lo;
    }
    return in == n ? 1 : in ? -1 : 0;
}

#define L2_TABLES 8
static u64 l2[L2_TABLES][PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));

/* Called from start.S with the MMU off and the device tree. Identity-maps
 * 0-1 TB: Normal write-back memory where the tree's /memory nodes say
 * there is RAM, Device memory everywhere else (MMIO, flash). 1 GB blocks,
 * or 2 MB blocks where a gigabyte holds both (the Raspberry Pi has RAM
 * from 0 and its peripherals just below 1 GB or 4 GB). Without a usable
 * tree, the QEMU virt layout: RAM from 1 GB to 256 GB. */
void arch_mmu_init(const void *dt)
{
    el = current_el();
    if (el != 1 && el != 2)
        return;                         /* EL3: arch_init reports it */
    ram_ranges(dt);
    if (!ram_count) {
        ram_base[0] = 1ULL << 30;
        ram_end[0] = 256ULL << 30;
        ram_count = 1;
    }
    u64 xn = el == 1 ? (XN | PXN) : XN;
    u64 ap = el == 2 ? AP_EL2_RES1 : 0;
    u64 normal = ATTR_NORMAL | SH_INNER | AF | ap, device = ATTR_DEVICE | xn | AF | ap;
    int tables = 0;
    for (u64 i = 0; i < 2 * PAGE_TABLE_ENTRIES; i++) {
        u64 pa = i << 30, d;
        int c = ram_cover(pa, 1ULL << 30);
        if (c < 0 && tables < L2_TABLES) {
            u64 *t = l2[tables++];
            for (u64 j = 0; j < PAGE_TABLE_ENTRIES; j++) {
                u64 b = pa + (j << 21);
                t[j] = b | DESC_BLOCK | (ram_cover(b, 1ULL << 21) == 1 ? normal : device);
            }
            d = (u64)(uptr)t | DESC_TABLE;
        } else
            d = pa | DESC_BLOCK | (c == 1 ? normal : device);
        l1[i / PAGE_TABLE_ENTRIES][i % PAGE_TABLE_ENTRIES] = d;
    }
    l0[0] = (u64)(uptr)l1[0] | DESC_TABLE;
    l0[1] = (u64)(uptr)l1[1] | DESC_TABLE;

    u64 pa_range = read_sysreg(id_aa64mmfr0_el1) & 0xF;
    if (pa_range > 5)
        pa_range = 5;                   /* 48 bits: no 52-bit descriptors here */
    u64 mair = 0xFF00;                  /* attr0 Device-nGnRnE, attr1 Normal WB RA WA */
    u64 tcr = 16 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12);  /* T0SZ 16, WB WA walks, inner shareable, 4 KB */
    __asm__ volatile("dsb sy");
    if (el == 1) {
        write_sysreg(mair_el1, mair);
        write_sysreg(tcr_el1, tcr | (1ULL << 23) | pa_range << 32);   /* EPD1, IPS */
        write_sysreg(ttbr0_el1, (uptr)l0);
        __asm__ volatile("isb; tlbi vmalle1; dsb ish; isb");
        u64 sctlr = read_sysreg(sctlr_el1);
        sctlr &= ~((1ULL << 1) | (1ULL << 19) | (1ULL << 25));       /* A, WXN, EE */
        sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);           /* M, C, I */
        write_sysreg(sctlr_el1, sctlr);
    } else {
        write_sysreg(mair_el2, mair);
        write_sysreg(tcr_el2, tcr | (1ULL << 23) | (1ULL << 31) | pa_range << 16);  /* RES1, PS */
        write_sysreg(ttbr0_el2, (uptr)l0);
        __asm__ volatile("isb; tlbi alle2; dsb ish; isb");
        u64 sctlr = read_sysreg(sctlr_el2);
        sctlr &= ~((1ULL << 1) | (1ULL << 19) | (1ULL << 25));
        sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
        write_sysreg(sctlr_el2, sctlr);
    }
    __asm__ volatile("isb; ic iallu; dsb ish; isb");
}

/* --------------------------------------------------------- exceptions */

void arch_exception(u64 entry)
{
    u64 esr, elr, far;
    if (el == 2) {
        esr = read_sysreg(esr_el2);
        elr = read_sysreg(elr_el2);
        far = read_sysreg(far_el2);
    } else {
        esr = read_sysreg(esr_el1);
        elr = read_sysreg(elr_el1);
        far = read_sysreg(far_el1);
    }
    printf("exception: EL%u vector %lu esr=%016lx elr=%016lx far=%016lx\n", el, entry, esr, elr, far);
    printf("boot halted\n");
    arch_halt();
}

/* ------------------------------------------------------------ platform */

/* reg entry i of node n as a CPU address, through the parents' ranges
 * (the Pi's peripherals are described at bus address 0x7E000000) */
static int reg(int n, int i, u64 *b)
{
    uint64_t a;
    if (n < 0 || dtb_reg(fdt_blob(), n, i, &a, NULL))
        return 0;
    *b = a;
    return 1;
}

static const char *implementer(u32 id)
{
    switch (id) {
    case 0x41: return "Arm";
    case 0x51: return "Qualcomm";
    case 0x61: return "Apple";
    case 0xC0: return "Ampere";
    default: return "other";
    }
}

static void find_devices(void)
{
    u64 b;
    int n = fdt_find_enabled("arm,pl011", -1);
    if (n >= 0 && reg(n, 0, &b) && b != uart) {
        uart = (uptr)b;
        arch_con_init();
    }
    n = fdt_find_enabled("arm,pl031", -1);
    if (n >= 0 && reg(n, 0, &b))
        rtc = (uptr)b;
    if ((n = fdt_find_compatible("arm,gic-v3", -1)) >= 0) {
        gic_version = 3;
        if (reg(n, 0, &b))
            gicd = (uptr)b;
        if (reg(n, 1, &b))
            gicr = (uptr)b;
    } else if ((n = fdt_find_compatible("arm,cortex-a15-gic", -1)) >= 0 ||
               (n = fdt_find_compatible("arm,gic-400", -1)) >= 0) {
        gic_version = 2;
        if (reg(n, 0, &b))
            gicd = (uptr)b;
        if (reg(n, 1, &b))
            gicr = (uptr)b;             /* the CPU interface on v2 */
    }
    if ((n = fdt_find_compatible("brcm,bcm2835-pm-wdt", -1)) >= 0 ||
        (n = fdt_find_compatible("brcm,bcm2835-pm", -1)) >= 0) {
        if (reg(n, 0, &b))
            pm_wdog = (uptr)b;
    }
    n = fdt_find_compatible("arm,psci-1.0", -1);
    if (n < 0)
        n = fdt_find_compatible("arm,psci-0.2", -1);
    if (n >= 0) {
        const char *m = fdt_prop(n, "method", NULL);
        if (m && !strcmp(m, "smc"))
            psci_smc = 1;
        else if (m && !strcmp(m, "hvc"))
            psci_smc = 0;
    }
}

/* GICv3: the distributor with affinity routing, every SPI in
 * non-secure group 1 at the middle priority and disabled; this CPU's
 * redistributor awake. QEMU's GIC has no secure side here (no EL3), so
 * the non-secure view is the whole GIC. Interrupts stay masked in PSTATE:
 * the firmware polls. */
#define GICD_CTLR    0x0000
#define GICD_TYPER   0x0004
#define GICD_IGROUPR 0x0080
#define GICD_ICENABLER 0x0180
#define GICD_IPRIORITYR 0x0400
#define GICD_PIDR2   0xFFE8
#define GICR_WAKER   0x0014
#define GICR_TYPER   0x0008

static void gic_init(void)
{
    if (!gicd) {
        printf("gic: none in the device tree\n");
        return;
    }
    u32 lines = ((mmio_read32(gicd + GICD_TYPER) & 0x1F) + 1) * 32;
    if (lines > 1020)
        lines = 1020;
    if (el3_active) {
        /* el3.c put every interrupt in non-secure group 1, enabled the
         * distributor and woke the redistributor: only the system
         * register interface is left at this EL */
        if (gic_version == 3) {
            if (el == 2)
                write_sysreg(S3_4_C12_C9_5, read_sysreg(S3_4_C12_C9_5) | 0x9);
            else
                write_sysreg(S3_0_C12_C12_5, read_sysreg(S3_0_C12_C12_5) | 0x1);
            __asm__ volatile("isb");
        }
        printf("gic: v%u, distributor at %p, %u interrupt lines, secure side set up at EL3\n",
               gic_version, (void *)gicd, lines);
        return;
    }
    if (gic_version == 3) {
        u32 arch = (mmio_read32(gicd + GICD_PIDR2) >> 4) & 0xF;   /* v2's 4 KB frame has none */
        mmio_write32(gicd + GICD_CTLR, 0);
        for (int i = 0; i < 100000 && (mmio_read32(gicd + GICD_CTLR) & (1u << 31)); i++)
            ;                                   /* RWP */
        for (u32 i = 32; i < lines; i += 32) {
            mmio_write32(gicd + GICD_ICENABLER + i / 8, 0xFFFFFFFF);
            mmio_write32(gicd + GICD_IGROUPR + i / 8, 0xFFFFFFFF);
        }
        for (u32 i = 32; i < lines; i += 4)
            mmio_write32(gicd + GICD_IPRIORITYR + i, 0xA0A0A0A0);
        mmio_write32(gicd + GICD_CTLR, (1u << 4) | (1u << 1));   /* ARE_NS, EnableGrp1NS */
        /* this CPU's redistributor: the frame whose GICR_TYPER affinity is ours */
        u64 mpidr = read_sysreg(mpidr_el1);
        u32 aff = (u32)((mpidr & 0xFFFFFF) | ((mpidr >> 8) & 0xFF000000));
        int awake = 0;
        for (uptr r = gicr; gicr && r < gicr + 0x20000 * 512; r += 0x20000) {
            u64 typer = mmio_read64(r + GICR_TYPER);
            if ((u32)(typer >> 32) == aff) {
                mmio_write32(r + GICR_WAKER, mmio_read32(r + GICR_WAKER) & ~2u);  /* ProcessorSleep */
                for (int i = 0; i < 100000 && (mmio_read32(r + GICR_WAKER) & 4); i++)
                    ;                           /* ChildrenAsleep */
                awake = !(mmio_read32(r + GICR_WAKER) & 4);
                break;
            }
            if (typer & (1 << 4))               /* Last */
                break;
        }
        if (el == 2)
            write_sysreg(S3_4_C12_C9_5, read_sysreg(S3_4_C12_C9_5) | 0x9);  /* ICC_SRE_EL2: SRE, Enable */
        else
            write_sysreg(S3_0_C12_C12_5, read_sysreg(S3_0_C12_C12_5) | 0x1);    /* ICC_SRE_EL1: SRE */
        __asm__ volatile("isb");
        printf("gic: v3 (arch %u), distributor at %p, %u interrupt lines, redistributor %s\n",
               arch, (void *)gicd, lines, awake ? "awake" : "not found");
    } else {
        printf("gic: v2, distributor at %p, CPU interface at %p, %u interrupt lines\n",
               (void *)gicd, (void *)gicr, lines);
    }
}

void arch_init(void)
{
    u64 midr = read_sysreg(midr_el1);
    u64 pa = read_sysreg(id_aa64mmfr0_el1) & 0xF;
    static const u8 pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    printf("cpu: %s part %03lx r%lup%lu, EL%u, %u-bit physical addresses\n",
           implementer((u32)(midr >> 24) & 0xFF), (midr >> 4) & 0xFFF, (midr >> 20) & 0xF, midr & 0xF,
           current_el(), pa < 7 ? pa_bits[pa] : 52);
    if (el != 1 && el != 2) {
        printf("cpu: started at EL%u, only EL1 and EL2 are supported, halting\n", current_el());
        arch_halt();
    }
    printf("mmu: identity map, caches on\n");
    if (el3_active)
        el3_report();
    find_devices();
    printf("uart: PL011 at %p\n", (void *)uart);
    if (psci_smc >= 0) {
        u64 v = psci(PSCI_VERSION, 0, 0, 0);
        printf("psci: v%lu.%lu via %s\n", (v >> 16) & 0xFFFF, v & 0xFFFF, psci_smc ? "smc" : "hvc");
    } else if (pm_wdog)
        printf("psci: none; reset and power-off through the watchdog at %p\n", (void *)pm_wdog);
    else
        printf("psci: none in the device tree\n");
    if (rtc)
        printf("rtc: PL031 at %p\n", (void *)rtc);
    gic_init();
}

void arch_memory_reserve(void)
{
    u64 dt = (uptr)fdt_blob(), size = fdt_size();
    efi_memory_mark(dt & ~0xFFFULL, ((dt & 0xFFF) + size + 0xFFF) / 0x1000, EfiBootServicesData);
    /* the tree's /memreserve/ entries (a boot stub, spin tables) */
    const u8 *h = fdt_blob();
    const u8 *r = h ? h + dtb_be32(h + 16) : NULL;
    for (int i = 0; r && i < 32; i++, r += 16) {
        u64 a = dtb_be64(r), n = dtb_be64(r + 8);
        if (!a && !n)
            break;
        efi_memory_mark(a & ~0xFFFULL, ((a & 0xFFF) + n + 0xFFF) / 0x1000, EfiReservedMemoryType);
    }
    /* RAM from 0 (the Pi): page 0 holds the boot stub and the other
     * CPUs' spin table, and must not become a null pointer anyway */
    if (ram_count && !ram_base[0])
        efi_memory_mark(0, 1, EfiReservedMemoryType);
    /* GetTime reads the RTC after ExitBootServices: the OS maps it */
    if (rtc)
        efi_memory_mark_attr(rtc & ~0xFFFULL, 1, EfiMemoryMappedIO, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
}

void arch_virtual_map(void)
{
    efi_convert_pointer(&rtc);
}

void arch_efi_init(void)
{
    arm_acpi_init();
}

/* For the memory self-test: 16 MB into the first RAM range */
unsigned long long arch_test_addr(void)
{
    for (int i = 0; i < mem_count; i++)
        if (mem_map[i].type == 1)
            return mem_map[i].base + 0x1000000;
    return 0x41000000ULL;
}

/* QEMU hands -kernel to firmware through fw_cfg here (main.c reads it) */
int arch_kernel(void **image, u32 *size)
{
    (void)image;
    (void)size;
    return 0;
}

/* Disks: virtio-mmio transports and the PCIe host bridge in the device tree */
int arch_disk_probe(void)
{
    return virtio_mmio_probe() + pci_probe() + sdhci_probe_dt();
}
