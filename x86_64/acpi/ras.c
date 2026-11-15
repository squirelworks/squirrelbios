/* SquirrelBIOS x86_64-uefi: error tables, BERT and HEST (roadmap F2).
 * Original code, written from the ACPI 6.5 specification (18.3.1 BERT,
 * 18.3.2 HEST, 18.3.2.1-2 IA-32 machine check and corrected machine
 * check sources, 18.3.2.7.1 Generic Error Status Block) and the UEFI
 * specification's CPER records (appendix N).
 *
 * BERT: the boot error region holds a Generic Error Status Block with
 * one Generic Error Data Entry per machine check found in the banks at
 * boot (mce.c), each an IA32/X64 processor error section whose context
 * is the bank's MSRs (CTL, STATUS, ADDR, MISC). With no error the block
 * status is 0.
 *
 * HEST: the IA-32 machine check source and the corrected machine check
 * source, with every bank; corrected errors are signalled by CMCI when
 * the CPU has it, else polled every second, with a threshold of 10 in a
 * minute before an OS should switch to polling. No firmware-first
 * handling: the OS owns the banks. */
#include "acpi.h"
#include "smm.h"

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

/* CPER section type: IA32/X64 processor error, DC3EA0B0-A144-4797-B95B-53FA242B6E1D */
static const u8 proc_ia_guid[16] = {
    0xb0, 0xa0, 0x3e, 0xdc, 0x44, 0xa1, 0x97, 0x47, 0xb9, 0x5b, 0x53, 0xfa, 0x24, 0x2b, 0x6e, 0x1d,
};

#define ENTRY_HDR   72                  /* Generic Error Data Entry, revision 3.0 */
#define SECTION     (64 + 16 + 32)      /* processor section, one MSR context of 4 */
#define BLOCK_HDR   20

static u32 build_region(u8 *region)
{
    const struct mce_record *r;
    int n = mce_records(&r);
    u8 *p = region + BLOCK_HDR;
    u32 status = 0, worst = 3;
    int ue = 0, ce = 0;
    for (int i = 0; i < n; i++) {
        u32 sev = mce_severity(&r[i]);
        if (sev == 2)
            ce++;
        else
            ue++;
        if (sev == 1 || (sev == 0 && worst != 1) || (sev == 2 && worst == 3))
            worst = sev;
        memcpy(p, proc_ia_guid, 16);
        put32(p + 16, sev);
        put16(p + 20, 0x0300);
        p[22] = 0;                      /* no FRU id / text */
        p[23] = 0;
        put32(p + 24, SECTION);
        u8 *s = p + ENTRY_HDR;
        put64(s, 1u | 2u | (0u << 2) | (1u << 8));   /* APIC ID, CPUID, 0 error infos, 1 context */
        put64(s + 8, r[i].apic_id);
        put64(s + 16, r[i].cpuid_eax);  /* CPUID.1 EAX in the first slot */
        u8 *ctx = s + 64;
        put16(ctx, 1);                  /* context type: MSR registers */
        put16(ctx + 2, 32);
        put32(ctx + 4, 0x400 + 4 * r[i].bank);
        put64(ctx + 8, 0);
        put64(ctx + 16, r[i].ctl);
        put64(ctx + 24, r[i].status);
        put64(ctx + 32, r[i].addr);
        put64(ctx + 40, r[i].misc);
        p += ENTRY_HDR + SECTION;
    }
    if (ue)
        status |= 1u | (ue > 1 ? 4u : 0);
    if (ce)
        status |= 2u | (ce > 1 ? 8u : 0);
    status |= (u32)(n & 0x3FF) << 4;
    put32(region, status);
    put32(region + 4, 0);               /* no raw data */
    put32(region + 8, 0);
    put32(region + 12, (u32)(p - region - BLOCK_HDR));
    put32(region + 16, n ? worst : 3);
    return (u32)(p - region);
}

/* Bank structures for both HEST sources */
static u8 *put_banks(u8 *p, u32 banks)
{
    for (u32 i = 0; i < banks; i++) {
        p[0] = (u8)i;
        p[1] = 0;                       /* clear status on init: the OS decides */
        p[2] = 0;                       /* IA-32 MCA status format */
        p[3] = 0;
        put32(p + 4, 0x400 + 4 * i);
        put64(p + 8, ~0ull);
        put32(p + 16, 0x401 + 4 * i);
        put32(p + 20, 0x402 + 4 * i);
        put32(p + 24, 0x403 + 4 * i);
        p += 28;
    }
    return p;
}

int acpi_ras_tables(u8 **out, acpi_take_fn take)
{
    mce_scan();
    u32 banks = mce_banks();
    const struct mce_record *r;
    int n = mce_records(&r);
    /* The region gets reserved pages of its own: the OS maps it
     * (Linux requests it as "APEI BERT"), which it can't inside the
     * ACPI tables' memory */
    u32 region_len = BLOCK_HDR + (u32)n * (ENTRY_HDR + SECTION);
    EFI_PHYSICAL_ADDRESS rbase = 0xFFFFFFFFull;
    if (efi_allocate_pages(AllocateMaxAddress, EfiReservedMemoryType, (region_len + 4095) / 4096, &rbase))
        return 0;
    u8 *region = (u8 *)(uptr)rbase;
    memset(region, 0, region_len);
    u8 *bert = take(48);
    u32 hest_len = 40 + (banks ? 40 + banks * 28 + 48 + banks * 28 : 0) + 64;
    u8 *hest = take(hest_len);
    if (!bert || !hest)
        return 0;

    build_region(region);
    put32(bert + 36, region_len);
    put64(bert + 40, (u64)(uptr)region);
    acpi_header(bert, "BERT", 48, 1);
    acpi_seal(bert);

    u8 *p = hest + 40;
    if (banks) {
        /* IA-32 machine check exception, source 0 */
        put16(p, 0);
        put16(p + 2, 0);
        p[6] = 0;                       /* not firmware first */
        p[7] = 1;                       /* enabled */
        put32(p + 8, 1);                /* records to pre-allocate */
        put32(p + 12, 1);               /* sections per record */
        put64(p + 16, mce_cap());
        put64(p + 24, ~0ull);           /* MCG_CTL */
        p[32] = (u8)banks;
        p = put_banks(p + 40, banks);
        /* corrected machine check, source 1 */
        int cmci = (mce_cap() >> 10) & 1;
        put16(p, 1);
        put16(p + 2, 1);
        p[6] = 0;
        p[7] = 1;
        put32(p + 8, 1);
        put32(p + 12, 1);
        u8 *nt = p + 16;                /* hardware error notification */
        nt[0] = cmci ? 5 : 0;           /* CMCI, or polled */
        nt[1] = 28;
        put16(nt + 2, 0);
        put32(nt + 4, cmci ? 0 : 1000); /* poll interval, ms */
        put32(nt + 8, 0);               /* vector */
        put32(nt + 12, 10);             /* switch to polling threshold value */
        put32(nt + 16, 60000);          /* ... window, ms */
        put32(nt + 20, 10);             /* error threshold value */
        put32(nt + 24, 60000);          /* error threshold window, ms */
        p[44] = (u8)banks;
        p = put_banks(p + 48, banks);
    }
    u32 sources = banks ? 2 : 0;
    /* The GHES that EINJ's SMI handler writes (smm.c), on q35 */
    u32 g = smm_hest_ghes(p, (u16)sources);
    p += g;
    sources += g ? 1 : 0;
    put32(hest + 36, sources);          /* error sources */
    acpi_header(hest, "HEST", (u32)(p - hest), 1);
    acpi_seal(hest);
    out[0] = bert;
    out[1] = hest;
    u8 *einj = smm_einj_table(take);
    if (!einj)
        return 2;
    out[2] = einj;
    return 3;
}
