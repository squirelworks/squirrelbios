/* SquirrelBIOS x86_64-uefi: machine-check banks (roadmap F2).
 * Original code, written from the Intel SDM (chapter 16, Machine-Check
 * Architecture) and the UEFI specification's CPER IA32/X64 processor
 * error section (appendix N).
 *
 * At boot every CPU reads its MCA banks. A bank whose MCi_STATUS is
 * valid holds an error from before the reset (machine checks survive a
 * warm reset); it is kept as a boot error record for the BERT and the
 * bank is cleared. Then the banks are enabled (MCG_CTL, MCi_CTL all
 * ones) and CR4.MCE is set, so a later fatal machine check resets the
 * machine and is found here on the next boot. */
#include "acpi.h"
#include "smp.h"

#define MSR_MCG_CAP   0x179
#define MSR_MCG_CTL   0x17B
#define MSR_MC0_CTL   0x400
#define MCG_CTL_P     (1u << 8)
#define MCG_CMCI_P    (1u << 10)
#define MCI_VAL       (1ull << 63)
#define MCI_UC        (1ull << 61)
#define MCI_MISCV     (1ull << 59)
#define MCI_ADDRV     (1ull << 58)
#define MCI_PCC       (1ull << 57)

static struct mce_record recs[MCE_MAX_RECORDS];
static volatile u32 nrecs, lost;
static u32 banks;
static u64 mcg_cap;
static int scanned;

static int has_mca(void)
{
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    return (d & (1u << 7)) && (d & (1u << 14));        /* MCE and MCA */
}

/* On each CPU: keep the valid banks, clear them, enable them */
static void EFIAPI scan_cpu(void *arg)
{
    (void)arg;
    if (!has_mca())
        return;
    u64 cap = rdmsr(MSR_MCG_CAP);
    u32 n = cap & 0xFF;
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    for (u32 i = 0; i < n; i++) {
        u64 st = rdmsr(MSR_MC0_CTL + 4 * i + 1);
        if (!(st & MCI_VAL))
            continue;
        u32 slot = __atomic_fetch_add(&nrecs, 1, __ATOMIC_RELAXED);
        if (slot < MCE_MAX_RECORDS) {
            struct mce_record *r = &recs[slot];
            r->apic_id = apic_id();
            r->bank = i;
            r->cpuid_eax = a;
            r->ctl = rdmsr(MSR_MC0_CTL + 4 * i);
            r->status = st;
            r->addr = (st & MCI_ADDRV) ? rdmsr(MSR_MC0_CTL + 4 * i + 2) : 0;
            r->misc = (st & MCI_MISCV) ? rdmsr(MSR_MC0_CTL + 4 * i + 3) : 0;
        } else {
            __atomic_fetch_add(&lost, 1, __ATOMIC_RELAXED);
        }
        wrmsr(MSR_MC0_CTL + 4 * i + 1, 0);
    }
    if (cap & MCG_CTL_P)
        wrmsr(MSR_MCG_CTL, ~0ull);
    for (u32 i = 0; i < n; i++)
        wrmsr(MSR_MC0_CTL + 4 * i, ~0ull);
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 | (1ull << 6)));
}

/* Once, on every CPU smp.c brought up */
void mce_scan(void)
{
    if (scanned)
        return;
    scanned = 1;
    if (!has_mca()) {
        printf("mce: no machine-check architecture\n");
        return;
    }
    mcg_cap = rdmsr(MSR_MCG_CAP);
    banks = mcg_cap & 0xFF;
    smp_run_all(scan_cpu, NULL);
    u32 n = nrecs < MCE_MAX_RECORDS ? nrecs : MCE_MAX_RECORDS;
    printf("mce: %u banks, CMCI %s, %u error%s from before reset\n", banks,
           (mcg_cap & MCG_CMCI_P) ? "yes" : "no", nrecs, nrecs == 1 ? "" : "s");
    for (u32 i = 0; i < n; i++)
        printf("mce: APIC ID %u bank %u status 0x%016lx addr 0x%lx misc 0x%lx (%s)\n",
               recs[i].apic_id, recs[i].bank, recs[i].status, recs[i].addr, recs[i].misc,
               mce_severity(&recs[i]) == 1 ? "fatal" :
               mce_severity(&recs[i]) == 0 ? "recoverable" : "corrected");
    if (lost)
        printf("mce: %u more not kept\n", lost);
}

u32 mce_banks(void) { return banks; }
u64 mce_cap(void) { return mcg_cap; }

int mce_records(const struct mce_record **out)
{
    *out = recs;
    return nrecs < MCE_MAX_RECORDS ? (int)nrecs : MCE_MAX_RECORDS;
}

/* CPER severity: 0 recoverable, 1 fatal, 2 corrected */
u32 mce_severity(const struct mce_record *r)
{
    if (!(r->status & MCI_UC))
        return 2;
    return (r->status & MCI_PCC) ? 1 : 0;
}
