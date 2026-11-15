/* SquirrelBIOS x86_64-uefi: the local APIC.
 * Original code, written from the Intel SDM's description of the APIC.
 *
 * xAPIC mode uses the registers at the APIC base (FEE00000h on QEMU),
 * x2APIC mode the MSRs from 800h. The firmware only reads its ID and
 * sends IPIs (INIT and STARTUP); interrupts stay off. At hand-off the
 * BSP's LVTs are set as a BIOS leaves them (apic_legacy_lvt). */
#include "smp.h"

#define MSR_APIC_BASE   0x1B
#define APIC_BASE_EN    (1u << 11)
#define APIC_BASE_EXTD  (1u << 10)
#define MSR_X2APIC_ID   0x802
#define MSR_X2APIC_SVR  0x80F
#define MSR_X2APIC_ICR  0x830

#define XAPIC_ID        0x020
#define XAPIC_SVR       0x0F0
#define XAPIC_ICR_LO    0x300
#define XAPIC_ICR_HI    0x310
#define ICR_PENDING     (1u << 12)
#define ICR_ALL_BUT_SELF (3u << 18)

static volatile u32 *xapic(u32 reg)
{
    u64 base = rdmsr(MSR_APIC_BASE) & 0xFFFFFF000ULL;
    return (volatile u32 *)(uptr)(base + reg);
}

int apic_has_x2apic(void)
{
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    return (c >> 21) & 1;
}

int apic_is_x2apic(void)
{
    return (rdmsr(MSR_APIC_BASE) & (APIC_BASE_EN | APIC_BASE_EXTD)) == (APIC_BASE_EN | APIC_BASE_EXTD);
}

/* Global enable, then x2APIC on top when asked (xAPIC must be enabled
 * first: going from disabled straight to x2APIC is invalid), then the
 * software enable bit in the spurious vector register */
void apic_enable(int x2)
{
    u64 base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_EN))
        wrmsr(MSR_APIC_BASE, base |= APIC_BASE_EN);
    if (x2 && !(base & APIC_BASE_EXTD))
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_EXTD);
    if (apic_is_x2apic())
        wrmsr(MSR_X2APIC_SVR, rdmsr(MSR_X2APIC_SVR) | 0x1FF);
    else
        *xapic(XAPIC_SVR) |= 0x1FF;
}

/* What a PC BIOS leaves on the BSP for an OS that drives the 8259s:
 * virtual wire mode, LINT0 ExtINT (the PIC's INTR) and LINT1 NMI, the
 * timer and error entries masked. The LVTs come out of reset masked, and
 * the APIC is enabled here, so without this no PIC interrupt arrives. */
#define LVT_MASKED      0x10000u
#define LVT_EXTINT      0x700u
#define LVT_NMI         0x400u
void apic_legacy_lvt(void)
{
    if (!(rdmsr(MSR_APIC_BASE) & APIC_BASE_EN))
        return;
    if (apic_is_x2apic()) {
        wrmsr(0x832, LVT_MASKED);       /* timer */
        wrmsr(0x835, LVT_EXTINT);       /* LINT0 */
        wrmsr(0x836, LVT_NMI);          /* LINT1 */
        wrmsr(0x837, LVT_MASKED);       /* error */
        return;
    }
    *xapic(0x320) = LVT_MASKED;
    *xapic(0x350) = LVT_EXTINT;
    *xapic(0x360) = LVT_NMI;
    *xapic(0x370) = LVT_MASKED;
}

u32 apic_id(void)
{
    if (apic_is_x2apic())
        return (u32)rdmsr(MSR_X2APIC_ID);
    return *xapic(XAPIC_ID) >> 24;
}

/* low: vector, delivery mode, level and trigger bits of the ICR */
void apic_send_ipi(u32 dest, u32 low, int all_but_self)
{
    if (all_but_self)
        low |= ICR_ALL_BUT_SELF;
    if (apic_is_x2apic()) {
        wrmsr(MSR_X2APIC_ICR, (u64)(all_but_self ? 0 : dest) << 32 | low);
        return;
    }
    *xapic(XAPIC_ICR_HI) = (all_but_self ? 0 : dest) << 24;
    *xapic(XAPIC_ICR_LO) = low;
    for (int n = 0; n < 1000000 && (*xapic(XAPIC_ICR_LO) & ICR_PENDING); n++)
        cpu_pause();
}
