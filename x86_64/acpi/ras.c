/* SquirrelBIOS x86_64-uefi: error tables, BERT and HEST (roadmap F2).
 * Original code, written from the ACPI 6.5 specification (18.3.1 BERT,
 * 18.3.2 HEST, 18.3.2.7.1 Generic Error Status Block).
 *
 * Stubs for now: the BERT points at a boot error region whose status
 * block says no error was recorded, and the HEST lists no error sources.
 * An OS that looks for them (Linux APEI) finds them well formed. */
#include "acpi.h"

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24);
}

#define REGION 64

int acpi_ras_tables(u8 **out, acpi_take_fn take)
{
    u8 *region = take(REGION);          /* block status 0: nothing recorded */
    u8 *bert = take(48);
    u8 *hest = take(40);
    if (!region || !bert || !hest)
        return 0;
    put32(bert + 36, REGION);
    u64 a = (u64)(uptr)region;
    put32(bert + 40, (u32)a);
    put32(bert + 44, (u32)(a >> 32));
    acpi_header(bert, "BERT", 48, 1);
    acpi_seal(bert);
    put32(hest + 36, 0);                /* error source count */
    acpi_header(hest, "HEST", 40, 1);
    acpi_seal(hest);
    out[0] = bert;
    out[1] = hest;
    return 2;
}
