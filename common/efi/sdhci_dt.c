/* SquirrelBIOS UEFI core (common/efi): SD host controllers named in the
 * device tree, for sdhci.c. Original code. The Raspberry Pi's
 * "brcm,bcm2711-emmc2" (Pi 4) and "brcm,bcm2835-sdhci" (Pi 1-3), and
 * plain "sdhci"; reg goes through the parents' ranges (the Pi's
 * peripherals sit at bus address 0x7E000000). A "clock-frequency"
 * property gives the base clock where the controller reports none. */
#include "efi.h"
#include "../fw/fdt.h"

/* Hardware vendor node: GUID, MMIO base; then SD(0) */
#define SQ_SDHCI_GUID \
    { 0x5371b1d3, 0x6d2c, 0x4f0e, { 0x9a, 0x38, 0x53, 0x44, 0x48, 0x43, 0x49, 0x00 } }

static void mmio_path(u64 b, u8 *p)
{
    static const EFI_GUID g = SQ_SDHCI_GUID;
    p[0] = HARDWARE_DEVICE_PATH;
    p[1] = 4;
    p[2] = 28;
    p[3] = 0;
    memcpy(p + 4, &g, 16);
    memcpy(p + 20, &b, 8);
    p[28] = END_DEVICE_PATH_TYPE;
    p[29] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[30] = 4;
    p[31] = 0;
}

static int enabled(const void *dt, int n)
{
    int len;
    const char *st = dtb_prop(dt, n, "status", &len);
    return !st || !strcmp(st, "okay") || !strcmp(st, "ok");
}

int sdhci_probe_dt(void)
{
    static const char *const compat[] = {
        "brcm,bcm2711-emmc2", "brcm,bcm2835-sdhci", "sdhci",
    };
    const void *dt = fdt_blob();
    int found = 0;
    for (unsigned i = 0; dt && i < sizeof compat / sizeof compat[0]; i++) {
        for (int n = dtb_find_compatible(dt, -1, compat[i]); n >= 0; n = dtb_find_compatible(dt, n, compat[i])) {
            uint64_t b;
            if (!enabled(dt, n) || dtb_reg(dt, n, 0, &b, NULL))
                continue;
            static char where[8][24];
            static int nwhere;
            char *w = where[nwhere++ & 7];
            memcpy(w, "MMIO 0x", 7);
            for (int k = 0; k < 10; k++)
                w[7 + k] = "0123456789abcdef"[(b >> (36 - 4 * k)) & 0xF];
            w[17] = 0;
            u8 path[32];
            mmio_path(b, path);
            u32 hz = dtb_prop_u32(dt, n, "clock-frequency", 0);
            found += sdhci_add_mmio((uptr)b, hz / 1000000, (EFI_DEVICE_PATH_PROTOCOL *)path, w);
        }
    }
    return found;
}
