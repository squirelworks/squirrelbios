/* SquirrelBIOS UEFI: USB configuration / device descriptor walks.
 * Original code. Used by xhci.c (mass-storage probe) and the host
 * fuzz harness (CONTINUANCE Phase G). Specs: USB 2.0 ch.9, BBB 1.0. */
#include "efi.h"
#include "usb_desc.h"

int usb_config_find_bot(const u8 *cfg, u32 total, struct usb_bot_info *out)
{
    int in_iface = 0;

    if (!cfg || !out || total < 9 || cfg[1] != 2)
        return 0;
    memset(out, 0, sizeof *out);
    out->config_value = cfg[5];

    for (u32 at = 0; at + 2 <= total && cfg[at] >= 2; at += cfg[at]) {
        const u8 *x = cfg + at;
        if ((u64)at + cfg[at] > total)
            break;
        if (x[1] == 4) {                    /* interface */
            in_iface = x[0] >= 9 && x[5] == 8 && x[6] == 6 && x[7] == 0x50;
        } else if (x[1] == 5 && in_iface && x[0] >= 7 && (x[3] & 3) == 2) {
            u32 num = x[2] & 0x0F;
            u32 mps = (u32)(x[4] | (x[5] & 7) << 8);
            if (x[2] & 0x80) {
                out->dci_in = 2 * num + 1;
                out->mps_in = mps;
            } else {
                out->dci_out = 2 * num;
                out->mps_out = mps;
            }
        }
    }
    return out->dci_in && out->dci_out;
}

int usb_parse_device_desc(const u8 *d, u32 len, u16 *vid, u16 *pid, u8 *mps0)
{
    if (!d || len < 8 || d[0] < 8 || d[1] != 1)
        return 0;
    if (mps0)
        *mps0 = d[7];
    if (len >= 12) {
        if (vid)
            *vid = (u16)(d[8] | d[9] << 8);
        if (pid)
            *pid = (u16)(d[10] | d[11] << 8);
    } else {
        if (vid)
            *vid = 0;
        if (pid)
            *pid = 0;
    }
    return 1;
}
