/* SquirrelBIOS x86_64-uefi: CXL memory enumeration (roadmap F2).
 * Original code, written from the CXL 2.0 / 3.0 specifications
 * (CEDT: CHBS and CFMWS; ACPI0016 host bridges and ACPI0017 CXL root
 * device), the ACPI 6.5 specification (table headers, DSDT devices), and
 * QEMU's documented CXL topology (`-M q35,cxl=on`, `pxb-cxl`, `cxl-rp`,
 * `cxl-type3`, `cxl-fmw`).
 *
 * QEMU places the CHBCR MMIO and each fixed memory window in guest PA
 * space and describes them in the CEDT it offers in fw_cfg
 * `etc/acpi/tables`. This firmware finds every `pxb-cxl` host bridge
 * (PCI 1b36:000b), takes that CEDT (CHBS + CFMWS), republishes it under
 * our OEM IDs, and puts `ACPI0016` (with `_CID` PNP0A08) on each CXL
 * root bus and an `ACPI0017` device in the DSDT so Linux's `cxl_acpi`
 * can bind. Without `cxl=on` there is no CEDT. */
#include "acpi.h"

#define CXL_PXB_VENDOR  0x1b36
#define CXL_PXB_DEVICE  0x000b
#define CXL_MAX_HB      6

#define CEDT_CHBS       0               /* CXL Host Bridge Structure */
#define CEDT_CFMWS      1               /* CXL Fixed Memory Window Structure */
#define CEDT_CHBS_LEN   32

struct cxl_hb {
    u32 uid;
    u64 base;
    u64 length;
};

static int present;
static int nhb;
static int ncfmws;
static struct cxl_hb hbs[CXL_MAX_HB];
static u16 cedt_sel;
static u32 cedt_off, cedt_len;

static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }

static int find_pxb_cxl(void)
{
    u8 bus, dev, fn;
    return pci_find(CXL_PXB_VENDOR, CXL_PXB_DEVICE, &bus, &dev, &fn);
}

/* Parse CHBS and count CFMWS in a CEDT body (bytes after the header) */
static int parse_cedt_body(const u8 *body, u32 len)
{
    nhb = 0;
    ncfmws = 0;
    for (u32 at = 0; at + 4 <= len;) {
        u8 type = body[at];
        u16 rlen = get16(body + at + 2);
        if (rlen < 4 || at + rlen > len)
            return 0;
        if (type == CEDT_CHBS) {
            if (rlen < CEDT_CHBS_LEN || nhb >= CXL_MAX_HB)
                return 0;
            hbs[nhb].uid = get32(body + at + 4);
            hbs[nhb].base = get64(body + at + 16);
            hbs[nhb].length = get64(body + at + 24);
            nhb++;
        } else if (type == CEDT_CFMWS) {
            ncfmws++;
        }
        at += rlen;
    }
    return nhb > 0 && ncfmws > 0;
}

/* Before the DSDT: find pxb-cxl and read QEMU's CEDT into the locals */
void acpi_cxl_probe(void)
{
    present = 0;
    nhb = 0;
    ncfmws = 0;
    cedt_len = 0;
    if (!find_pxb_cxl())
        return;
    if (!acpi_qemu_table("CEDT", &cedt_sel, &cedt_off, &cedt_len) ||
        cedt_len < 36 + CEDT_CHBS_LEN) {
        printf("cxl: pxb-cxl present, but no CEDT in etc/acpi/tables\n");
        return;
    }
    /* Parse in a small stack buffer when the table fits; else page it */
    u8 tmp[512];
    if (cedt_len > sizeof tmp) {
        printf("cxl: CEDT too large (%u bytes)\n", cedt_len);
        return;
    }
    fwcfg_read_at(cedt_sel, cedt_off, tmp, cedt_len);
    if (memcmp(tmp, "CEDT", 4) != 0 || get32(tmp + 4) != cedt_len) {
        printf("cxl: CEDT in fw_cfg is unreadable\n");
        return;
    }
    if (!parse_cedt_body(tmp + 36, cedt_len - 36)) {
        printf("cxl: CEDT needs one CHBS and one CFMWS\n");
        return;
    }
    present = 1;
    printf("cxl: %d host bridge%s (ACPI0016), %d fixed window%s",
           nhb, nhb == 1 ? "" : "s", ncfmws, ncfmws == 1 ? "" : "s");
    for (int i = 0; i < nhb; i++)
        printf("%s CHBS uid %u at 0x%llx/0x%llx", i ? "," : ":",
               hbs[i].uid, (unsigned long long)hbs[i].base,
               (unsigned long long)hbs[i].length);
    printf("\n");
}

int acpi_cxl_present(void)
{
    return present;
}

int acpi_cxl_is_hb(u8 bus)
{
    for (int i = 0; i < nhb; i++)
        if (hbs[i].uid == bus)
            return 1;
    return 0;
}

int acpi_cxl_chbs(u8 bus, u64 *base, u64 *length)
{
    for (int i = 0; i < nhb; i++)
        if (hbs[i].uid == bus) {
            if (base)
                *base = hbs[i].base;
            if (length)
                *length = hbs[i].length;
            return 1;
        }
    return 0;
}

void acpi_cxl_dsdt(struct aml *a)
{
    if (!present)
        return;
    aml_device(a, "CXLM");
    aml_name_string(a, "_HID", "ACPI0017");
    aml_name_int(a, "_STA", 0x0F);
    aml_end(a);
}

int acpi_cxl_tables(u8 **out, acpi_take_fn take)
{
    if (!present || !cedt_len)
        return 0;
    u8 *t = take(cedt_len);
    if (!t)
        return 0;
    fwcfg_read_at(cedt_sel, cedt_off, t, cedt_len);
    acpi_header(t, "CEDT", cedt_len, 1);
    acpi_seal(t);
    out[0] = t;
    return 1;
}
