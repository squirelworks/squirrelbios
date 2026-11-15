/* SquirrelBIOS aarch64-uefi: a small EL3 monitor for when the firmware
 * itself starts at EL3 (QEMU virt with secure=on), which is when QEMU
 * leaves PSCI and the secure side of the GIC to the firmware. Original
 * code, written from the Arm Architecture Reference Manual (A-profile),
 * the Arm Power State Coordination Interface (PSCI, DEN0022), the SMC
 * Calling Convention (DEN0028), the GICv2 and GICv3 architecture
 * specifications and the PL061 GPIO technical reference manual.
 *
 * Every CPU starts at the reset vector. The boot CPU (affinity 0) runs
 * el3_init: the secure configuration (SCR_EL3, CPTR_EL3), the GIC's
 * secure side (every interrupt in non-secure group 1), and a /psci node
 * with enable-method = "psci" on the CPUs added to the device tree. It
 * then drops to non-secure EL2 (or EL1 without EL2) and the firmware
 * carries on as it does there. The other CPUs wait in a holding pen
 * (start.S) until PSCI CPU_ON gives them an entry point.
 *
 * PSCI 1.1 through smc: VERSION, CPU_SUSPEND (standby only), CPU_OFF,
 * CPU_ON, AFFINITY_INFO, MIGRATE_INFO_TYPE, SYSTEM_OFF, SYSTEM_RESET,
 * PSCI_FEATURES. Power off and reset use the GPIO lines the device tree
 * gives "gpio-poweroff" and "gpio-restart" (QEMU's secure PL061).
 *
 * The monitor runs with its MMU off. Its code and data are part of the
 * firmware image, which the UEFI memory map keeps as runtime services
 * code, so an OS leaves them alone; they are not in secure-only memory,
 * so this is a PSCI provider, not a security boundary. */
#include "efi.h"
#include "../../common/fw/fdt.h"

#define read_sysreg(r) ({ u64 _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((u64)(v)))

#define EL3_CPUS 64

/* One per CPU, indexed by el3_cpu_index(): start.S reads entry */
struct el3_cpu {
    u64 entry;          /* non-zero: leave the pen for it */
    u64 context;
    u64 mpidr;          /* from the device tree; ~0 if no such CPU */
    u32 state;          /* PSCI AFFINITY_INFO: 0 on, 1 off, 2 on pending */
    u32 pad;
};
struct el3_cpu el3_cpus[EL3_CPUS];
/* Per-CPU monitor stacks (start.S sets SP_EL3 from these) */
u8 el3_stacks[EL3_CPUS][2048] __attribute__((aligned(16)));

int el3_active;                 /* the boot CPU went through el3_init */
u32 el3_ncpus;
static uptr gicd, gicr, gicc;
static u32 gic_version;
static uptr gpio_off_base, gpio_reset_base;
static int gpio_off_line = -1, gpio_reset_line = -1;
static u64 spsr_ns;
const char *el3_dt_note = "";

#define PSCI_SUCCESS          0
#define PSCI_NOT_SUPPORTED    (-1)
#define PSCI_INVALID_PARAMS   (-2)
#define PSCI_DENIED           (-3)
#define PSCI_ALREADY_ON       (-4)
#define PSCI_ON_PENDING       (-5)

#define PSCI_VERSION          0x84000000
#define PSCI_CPU_SUSPEND      0x84000001
#define PSCI_CPU_OFF          0x84000002
#define PSCI_CPU_ON           0x84000003
#define PSCI_AFFINITY_INFO    0x84000004
#define PSCI_MIGRATE_INFO     0x84000006
#define PSCI_SYSTEM_OFF       0x84000008
#define PSCI_SYSTEM_RESET     0x84000009
#define PSCI_FEATURES         0x8400000A

#define CPU_ON  0
#define CPU_OFF 1
#define CPU_PENDING 2

/* Affinity 0 and 1 make the index; start.S computes the same */
static int cpu_index(u64 mpidr)
{
    if (mpidr & 0xFF000000FF0000ULL)
        return -1;
    u32 i = (u32)((mpidr >> 8 & 0xFF) * 16 + (mpidr & 0xFF));
    return (mpidr & 0xFF) < 16 && i < EL3_CPUS ? (int)i : -1;
}

static int find_cpu(u64 mpidr)
{
    int i = cpu_index(mpidr & 0xFF00FFFFFFULL);
    return i >= 0 && el3_cpus[i].mpidr == (mpidr & 0xFF00FFFFFFULL) ? i : -1;
}

static int has_el2(void)
{
    return ((read_sysreg(id_aa64pfr0_el1) >> 8) & 0xF) != 0;
}

/* SCR_EL3 and the trap controls, the same on every CPU */
static void secure_config(void)
{
    u64 pfr0 = read_sysreg(id_aa64pfr0_el1);
    u64 pfr1 = read_sysreg(id_aa64pfr1_el1);
    u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
    u64 mmfr1 = read_sysreg(id_aa64mmfr1_el1);
    u64 isar1 = read_sysreg(id_aa64isar1_el1);
    /* NS, RES1 bits 4-5, HCE (hvc on), RW (lower ELs AArch64); smc stays on */
    u64 scr = 1 | (3 << 4) | (1 << 10);
    if (has_el2())
        scr |= 1 << 8;
    if (isar1 & 0xFF0000FF0ULL)                 /* APA/API/GPA/GPI: pointer auth */
        scr |= (1ULL << 16) | (1ULL << 17);     /* API, APK: no traps */
    if (((pfr0 >> 56) & 0xF) >= 2)
        scr |= 1ULL << 25;                      /* EnSCXT */
    if (((pfr1 >> 8) & 0xF) >= 2)
        scr |= 1ULL << 26;                      /* ATA (MTE) */
    if ((mmfr0 >> 56) & 0xF)
        scr |= 1ULL << 27;                      /* FGTEn */
    if ((mmfr0 >> 60) & 0xF)
        scr |= 1ULL << 28;                      /* ECVEn */
    if ((mmfr1 >> 40) & 0xF)
        scr |= 1ULL << 38;                      /* HXEn */
    if ((pfr1 >> 24) & 0xF)
        scr |= 1ULL << 41;                      /* EnTP2 (SME's TPIDR2_EL0) */
    u64 mmfr3 = read_sysreg(S3_0_C0_C7_3);      /* ID_AA64MMFR3_EL1 */
    if (mmfr3 & 0xF)
        scr |= 1ULL << 43;                      /* TCR2En */
    if ((mmfr3 >> 4) & 0xF)
        scr |= 1ULL << 44;                      /* SCTLR2En */
    if ((mmfr3 >> 8) & 0xF)
        scr |= 1ULL << 45;                      /* PIEn */
    write_sysreg(scr_el3, scr);
    u64 cptr = 0;                               /* TFP, TTA, TCPAC clear */
    if ((pfr0 >> 32) & 0xF)
        cptr |= 1 << 8;                         /* EZ: SVE */
    if ((pfr1 >> 24) & 0xF)
        cptr |= 1 << 12;                        /* ESM: SME */
    write_sysreg(cptr_el3, cptr);
    __asm__ volatile("isb");
    if ((pfr0 >> 32) & 0xF)
        write_sysreg(S3_6_C1_C2_0, 0xF);        /* ZCR_EL3: the longest vectors */
    if ((pfr1 >> 24) & 0xF)
        write_sysreg(S3_6_C1_C2_6, 0xF);        /* SMCR_EL3 */
    /* the statistical profiling and trace buffers belong to the
     * non-secure side, the rest of the debug and PMU stays untrapped */
    u64 dfr0 = read_sysreg(id_aa64dfr0_el1), mdcr = 0;
    if ((dfr0 >> 32) & 0xF)
        mdcr |= 3ULL << 12;                     /* NSPB: SPE */
    if ((dfr0 >> 44) & 0xF)
        mdcr |= 3ULL << 24;                     /* NSTB: TRBE */
    write_sysreg(mdcr_el3, mdcr);
    /* lower ELs start with their MMU off and the RES1 bits set */
    if (has_el2()) {
        write_sysreg(sctlr_el2, 0x30C50830);
        write_sysreg(hcr_el2, 1ULL << 31);      /* RW */
        write_sysreg(cntvoff_el2, 0);
    }
    write_sysreg(sctlr_el1, 0x30D00800);
    __asm__ volatile("isb");
}

/* --------------------------------------------------------------- GIC */

#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_IGRPMODR   0x0D00
#define GICR_WAKER      0x0014
#define GICR_TYPER      0x0008
#define GICR_SGI        0x10000
#define GICC_CTLR       0x0000
#define GICC_PMR        0x0004

/* This CPU's banked part: SGIs and PPIs into non-secure group 1, its
 * redistributor awake (GICv3), the CPU interface's system registers on
 * for the lower ELs */
static void gic_cpu(void)
{
    if (gic_version == 3) {
        u64 mpidr = read_sysreg(mpidr_el1);
        u32 aff = (u32)((mpidr & 0xFFFFFF) | ((mpidr >> 8) & 0xFF000000));
        for (uptr r = gicr; gicr && r < gicr + 0x20000 * 512; r += 0x20000) {
            u64 typer = mmio_read64(r + GICR_TYPER);
            if ((u32)(typer >> 32) == aff) {
                mmio_write32(r + GICR_SGI + GICD_IGROUPR, 0xFFFFFFFF);
                mmio_write32(r + GICR_SGI + GICD_IGRPMODR, 0);
                mmio_write32(r + GICR_WAKER, mmio_read32(r + GICR_WAKER) & ~2u);
                for (int i = 0; i < 100000 && (mmio_read32(r + GICR_WAKER) & 4); i++)
                    ;
                break;
            }
            if (typer & (1 << 4))
                break;
        }
        /* ICC_SRE_EL3: SRE, DFB, DIB, Enable (lower ELs may use theirs) */
        write_sysreg(S3_6_C12_C12_5, 0xF);
        __asm__ volatile("isb");
    } else if (gic_version == 2) {
        mmio_write32(gicd + GICD_IGROUPR, 0xFFFFFFFF);      /* banked: SGIs, PPIs */
        if (gicc) {
            mmio_write32(gicc + GICC_PMR, 0xFF);
            mmio_write32(gicc + GICC_CTLR, 0x3);            /* secure view: both groups */
        }
    }
}

static void gic_init(void)
{
    if (!gicd)
        return;
    u32 lines = ((mmio_read32(gicd + GICD_TYPER) & 0x1F) + 1) * 32;
    if (lines > 1020)
        lines = 1020;
    if (gic_version == 3) {
        mmio_write32(gicd + GICD_CTLR, 0);
        for (int i = 0; i < 100000 && (mmio_read32(gicd + GICD_CTLR) & (1u << 31)); i++)
            ;
        mmio_write32(gicd + GICD_CTLR, (1u << 4) | (1u << 5));      /* ARE_S, ARE_NS */
        for (u32 i = 32; i < lines; i += 32) {
            mmio_write32(gicd + GICD_IGROUPR + i / 8, 0xFFFFFFFF);
            mmio_write32(gicd + GICD_IGRPMODR + i / 8, 0);
        }
        mmio_write32(gicd + GICD_CTLR, (1u << 4) | (1u << 5) | (1u << 1));  /* + EnableGrp1NS */
        for (int i = 0; i < 100000 && (mmio_read32(gicd + GICD_CTLR) & (1u << 31)); i++)
            ;
    } else {
        mmio_write32(gicd + GICD_CTLR, 0);
        for (u32 i = 32; i < lines; i += 32)
            mmio_write32(gicd + GICD_IGROUPR + i / 8, 0xFFFFFFFF);
        mmio_write32(gicd + GICD_CTLR, 0x3);                        /* both groups */
    }
}

/* ------------------------------------------------------- device tree */

static int okay(const void *dt, int node)
{
    int len;
    const char *s = dtb_prop(dt, node, "status", &len);
    return !s || !strcmp(s, "okay") || !strcmp(s, "ok");
}

/* gpio-poweroff / gpio-restart: <&controller line flags> on a PL061;
 * secure-only nodes say status = "disabled", secure-status = "okay" */
static void find_gpio(const void *dt, const char *compat, uptr *base, int *line)
{
    int n = dtb_find_compatible(dt, -1, compat);
    int len;
    const u8 *p = n >= 0 ? dtb_prop(dt, n, "gpios", &len) : NULL;
    if (!p || len < 8)
        return;
    int c = dtb_by_phandle(dt, dtb_be32(p));
    u64 b;
    if (c < 0 || !dtb_compatible(dt, c, "arm,pl061") || !fdt_reg(c, 0, &b, NULL))
        return;
    *base = (uptr)b;
    *line = (int)dtb_be32(p + 4);
}

static void find_devices(const void *dt)
{
    u64 b;
    int n;
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
            gicc = (uptr)b;
    }
    find_gpio(dt, "gpio-poweroff", &gpio_off_base, &gpio_off_line);
    find_gpio(dt, "gpio-restart", &gpio_reset_base, &gpio_reset_line);
    for (int i = 0; i < EL3_CPUS; i++) {
        el3_cpus[i].mpidr = ~0ULL;
        el3_cpus[i].state = CPU_OFF;
    }
    int cpus = dtb_path(dt, "/cpus");
    u32 ac = cpus >= 0 ? dtb_prop_u32(dt, cpus, "#address-cells", 1) : 1;
    for (int c = cpus >= 0 ? dtb_first_child(dt, cpus) : -1; c >= 0; c = dtb_next_sibling(dt, c)) {
        int len;
        const char *type = dtb_prop(dt, c, "device_type", &len);
        const u8 *reg = dtb_prop(dt, c, "reg", &len);
        if (!type || strcmp(type, "cpu") || !reg || len < (int)(ac * 4) || !okay(dt, c))
            continue;
        u64 mpidr = dtb_cells(reg, (int)ac);
        int i = cpu_index(mpidr);
        if (i >= 0) {
            el3_cpus[i].mpidr = mpidr;
            el3_ncpus++;
        }
    }
}

/* Adds what QEMU leaves out when the firmware owns EL3: the /psci node
 * and enable-method = "psci" on each CPU. Rewritten in place: the tree
 * sits at the start of RAM with the rest of the first megabyte free. */
static u8 dt_buf[0x10000];

static int skip_enable(const void *src, int node, const char *prop, void *ctx)
{
    (void)ctx;
    int len;
    const char *type = dtb_prop(src, node, "device_type", &len);
    return type && !strcmp(type, "cpu") && !strcmp(prop, "enable-method");
}

static void add_psci(struct fdt_writer *w, const void *src, int node, void *ctx)
{
    (void)ctx;
    int len;
    const char *type = dtb_prop(src, node, "device_type", &len);
    if (type && !strcmp(type, "cpu"))
        fdtw_prop_str(w, "enable-method", "psci");
    if (node == dtb_root(src)) {
        fdtw_begin(w, "psci");
        fdtw_prop_str2(w, "compatible", "arm,psci-1.0", "arm,psci-0.2");
        fdtw_prop_str(w, "method", "smc");
        fdtw_end(w);
    }
}

static void edit_tree(void *dt)
{
    if (dtb_path(dt, "/psci") >= 0) {
        el3_dt_note = ", /psci already in the device tree";
        return;
    }
    struct fdt_writer w;
    fdtw_init(&w, dt_buf, sizeof dt_buf);
    /* the reserved-memory map is carried over */
    const u8 *h = dt;
    const u8 *rsv = h + dtb_be32(h + 16);
    for (int i = 0; i < 16; i++, rsv += 16) {
        u64 a = dtb_be64(rsv), s = dtb_be64(rsv + 8);
        if (!a && !s)
            break;
        fdtw_reserve(&w, a, s);
    }
    fdtw_copy(&w, dt, dtb_root(dt), add_psci, skip_enable, NULL);
    u32 size = fdtw_finish(&w, dtb_be32(h + 28));
    if (!size || size > 0x100000 - 0x1000) {
        el3_dt_note = ", device tree NOT updated (no room)";
        return;
    }
    memcpy(dt, dt_buf, size);
    el3_dt_note = ", /psci added to the device tree";
}

/* -------------------------------------------------------- entry points */

/* Boot CPU, from start.S with the image in RAM and the MMU off; returns
 * the SPSR for the drop to non-secure EL2 (or EL1) */
u64 el3_init(void *dt)
{
    secure_config();
    if (fdt_init(dt)) {
        find_devices(dt);
        edit_tree(dt);
        fdt_init(dt);
    }
    gic_init();
    gic_cpu();
    int me = cpu_index(read_sysreg(mpidr_el1) & 0xFF00FFFFFFULL);
    if (me >= 0)
        el3_cpus[me].state = CPU_ON;
    el3_active = 1;
    spsr_ns = has_el2() ? 0x3C9 : 0x3C5;        /* EL2h or EL1h, DAIF masked */
    return spsr_ns;
}

/* A CPU leaving the pen (start.S): its own secure setup; returns the SPSR */
u64 el3_cpu_start(u64 index)
{
    secure_config();
    gic_cpu();
    el3_cpus[index].state = CPU_ON;
    return spsr_ns;
}

void el3_park(u64 index) __attribute__((noreturn));     /* start.S */

static void gpio_pulse(uptr base, int line)
{
    if (!base || line < 0 || line > 7)
        return;
    u32 bit = 1u << line;
    mmio_write32(base + 0x400, mmio_read32(base + 0x400) | bit);   /* GPIODIR: output */
    mmio_write32(base + (bit << 2), 0);
    mmio_write32(base + (bit << 2), bit);                          /* rising edge */
}

static s64 cpu_on(u64 target, u64 entry, u64 context)
{
    int i = find_cpu(target);
    if (i < 0)
        return PSCI_INVALID_PARAMS;
    struct el3_cpu *c = &el3_cpus[i];
    if (c->state == CPU_ON)
        return PSCI_ALREADY_ON;
    if (c->state == CPU_PENDING)
        return PSCI_ON_PENDING;
    c->state = CPU_PENDING;
    c->context = context;
    __asm__ volatile("dsb sy");
    c->entry = entry;
    __asm__ volatile("dc civac, %0; dsb sy; sev" :: "r"(c) : "memory");
    return PSCI_SUCCESS;
}

static s64 affinity_info(u64 target, u64 level)
{
    if (level)
        return PSCI_INVALID_PARAMS;     /* only single CPUs are reported */
    int i = find_cpu(target);
    return i < 0 ? PSCI_INVALID_PARAMS : (s64)el3_cpus[i].state;
}

static s64 features(u32 fn)
{
    switch (fn & ~0x40000000u) {
    case PSCI_VERSION: case PSCI_CPU_SUSPEND: case PSCI_CPU_OFF: case PSCI_CPU_ON:
    case PSCI_AFFINITY_INFO: case PSCI_MIGRATE_INFO: case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET: case PSCI_FEATURES:
        return PSCI_SUCCESS;            /* CPU_SUSPEND: original power_state format */
    }
    return PSCI_NOT_SUPPORTED;
}

static void system_power(int off) __attribute__((noreturn));
static void system_power(int off)
{
    /* a reset must not find the pen open from this boot */
    for (int i = 0; i < EL3_CPUS; i++)
        el3_cpus[i].entry = 0;
    __asm__ volatile("dsb sy");
    if (off)
        gpio_pulse(gpio_off_base, gpio_off_line);
    else
        gpio_pulse(gpio_reset_base, gpio_reset_line);
    for (;;)
        __asm__ volatile("wfi");
}

/* The smc handler (start.S): x[0..3] in, x[0] out. SMC64 and SMC32
 * function IDs are both taken for the PSCI calls. */
void el3_smc(u64 *x)
{
    u32 fn = (u32)x[0];
    u32 call = fn & ~0x40000000u;
    int is64 = fn & 0x40000000u;
    u64 a1 = is64 ? x[1] : (u32)x[1], a2 = is64 ? x[2] : (u32)x[2], a3 = is64 ? x[3] : (u32)x[3];
    s64 r = PSCI_NOT_SUPPORTED;
    switch (call) {
    case PSCI_VERSION:
        r = 0x10001;
        break;
    case PSCI_CPU_SUSPEND:
        if (a1 & (1 << 16))
            r = PSCI_DENIED;            /* power-down states: none */
        else {
            __asm__ volatile("dsb sy; wfi");
            r = PSCI_SUCCESS;
        }
        break;
    case PSCI_CPU_OFF: {
        int i = cpu_index(read_sysreg(mpidr_el1) & 0xFF00FFFFFFULL);
        if (i < 0) {
            r = PSCI_DENIED;
            break;
        }
        el3_cpus[i].entry = 0;
        el3_cpus[i].state = CPU_OFF;
        __asm__ volatile("dsb sy");
        el3_park((u64)i);
    }
    case PSCI_CPU_ON:
        r = cpu_on(a1, a2, a3);
        break;
    case PSCI_AFFINITY_INFO:
        r = affinity_info(a1, a2);
        break;
    case PSCI_MIGRATE_INFO:
        r = 2;                          /* no trusted OS: migration not needed */
        break;
    case PSCI_SYSTEM_OFF:
        system_power(1);
    case PSCI_SYSTEM_RESET:
        system_power(0);
    case PSCI_FEATURES:
        r = features((u32)a1);
        break;
    }
    x[0] = (u64)r;
}

/* Anything else reaching EL3 (start.S): report on the console and stop */
void el3_unexpected(u64 vector)
{
    printf("el3: unexpected exception, vector %lu esr=%016lx elr=%016lx far=%016lx, halting\n",
           vector, read_sysreg(esr_el3), read_sysreg(elr_el3), read_sysreg(far_el3));
    for (;;)
        __asm__ volatile("wfi");
}

/* Non-secure side (arch.c): a line for the boot log */
void el3_report(void)
{
    printf("el3: PSCI 1.1 monitor, %u CPUs, GICv%u secure side set up, power %s, reset %s%s\n",
           el3_ncpus, gic_version, gpio_off_base ? "by GPIO" : "none", gpio_reset_base ? "by GPIO" : "none",
           el3_dt_note);
}
