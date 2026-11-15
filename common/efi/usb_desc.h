/* SquirrelBIOS UEFI: USB descriptor helpers shared by xHCI and host fuzz.
 * Original code, from USB 2.0 chapter 9 and USB Mass Storage BBB 1.0. */
#ifndef USB_DESC_H
#define USB_DESC_H

#include "sq.h"

/* Bulk-Only Transport (class 8 / subclass 6 / protocol 0x50) endpoints
 * found in a configuration descriptor chain. */
struct usb_bot_info {
    u8 config_value;
    u32 dci_in;     /* xHCI device context index for bulk IN */
    u32 dci_out;    /* xHCI device context index for bulk OUT */
    u32 mps_in;
    u32 mps_out;
};

/* Walk a configuration descriptor (bDescriptorType 2 at [1]) for a BBB
 * interface and its bulk IN/OUT endpoints. 1 when both endpoints exist.
 * total is the wTotalLength budget; descriptors past it are ignored. */
int usb_config_find_bot(const u8 *cfg, u32 total, struct usb_bot_info *out);

/* Device descriptor (type 1): 1 when bLength/type look sane; optional
 * vid/pid/mps0 filled when non-NULL and enough bytes are present. */
int usb_parse_device_desc(const u8 *d, u32 len, u16 *vid, u16 *pid, u8 *mps0);

#endif
