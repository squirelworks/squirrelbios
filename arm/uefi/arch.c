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

extern char __fw_start[], __fw_end[];

static uptr uart = CONFIG_UART_BASE;
static uptr rtc;
static uptr gicd, gicr;
static u32 gic_version;
static int psci_smc = -1;               /* -1: no PSCI, 0: hvc, 1: smc */
static u32 el;

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

void arch_con_init(void)
{
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

void arch_reset(int type, u64 status)
{
    (void)status;
    if (psci_smc >= 0)
        psci(type == EfiResetShutdown ? PSCI_SYSTEM_OFF : PSCI_SYSTEM_RESET, 0, 0, 0);
    if (!efi_boot_services_exited())
        printf("fw: no PSCI to %s with, halting\n", type == EfiResetShutdown ? "power off" : "reset");
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

/* Called from start.S with the MMU off. Identity-maps 0-1 TB with 1 GB
 * blocks: Device for 0-1 GB (flash, MMIO) and from 256 GB (high PCIe),
 * Normal memory for 1-256 GB (where virt puts RAM). */
void arch_mmu_init(void)
{
    el = current_el();
    if (el != 1 && el != 2)
        return;                         /* EL3: arch_init reports it */
    u64 xn = el == 1 ? (XN | PXN) : XN;
    u64 ap = el == 2 ? AP_EL2_RES1 : 0;
    for (u64 i = 0; i < 2 * PAGE_TABLE_ENTRIES; i++) {
        u64 pa = i << 30;
        u64 d = pa | DESC_BLOCK | AF | ap;
        if (i >= 1 && i < 256)
            d |= ATTR_NORMAL | SH_INNER;
        else
            d |= ATTR_DEVICE | xn;
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
    int n = fdt_find_compatible("arm,pl011", -1);
    if (n >= 0 && fdt_reg(n, 0, &b, NULL) && b != uart) {
        uart = (uptr)b;
        arch_con_init();
    }
    n = fdt_find_compatible("arm,pl031", -1);
    if (n >= 0 && fdt_reg(n, 0, &b, NULL))
        rtc = (uptr)b;
    if ((n = fdt_find_compatible("arm,gic-v3", -1)) >= 0) {
        gic_version = 3;
        if (fdt_reg(n, 0, &b, NULL))
            gicd = (uptr)b;
        if (fdt_reg(n, 1, &b, NULL))
            gicr = (uptr)b;
    } else if ((n = fdt_find_compatible("arm,cortex-a15-gic", -1)) >= 0 ||
               (n = fdt_find_compatible("arm,gic-400", -1)) >= 0) {
        gic_version = 2;
        if (fdt_reg(n, 0, &b, NULL))
            gicd = (uptr)b;
        if (fdt_reg(n, 1, &b, NULL))
            gicr = (uptr)b;             /* the CPU interface on v2 */
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
    find_devices();
    printf("uart: PL011 at %p\n", (void *)uart);
    if (psci_smc >= 0) {
        u64 v = psci(PSCI_VERSION, 0, 0, 0);
        printf("psci: v%lu.%lu via %s\n", (v >> 16) & 0xFFFF, v & 0xFFFF, psci_smc ? "smc" : "hvc");
    } else
        printf("psci: none in the device tree\n");
    if (rtc)
        printf("rtc: PL031 at %p\n", (void *)rtc);
    gic_init();
}

void arch_memory_reserve(void)
{
    u64 dt = ARCH_DTB_ADDR, size = fdt_size();
    efi_memory_mark(dt & ~0xFFFULL, ((dt & 0xFFF) + size + 0xFFF) / 0x1000, EfiBootServicesData);
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
}
