/* SquirrelBIOS x86_64-uefi: QEMU firmware configuration device (fw_cfg),
 * through its I/O ports. Original code, written from QEMU's documented
 * interface (docs/specs/fw_cfg.rst): a 16-bit selector at 0x510, a byte
 * data port at 0x511, the signature "QEMU" at selector 0, and the file
 * directory at selector 0x19 (big-endian count, then 64-byte entries:
 * size, select, reserved, 56-byte name). */
#include "sq.h"

#define FWCFG_SEL   0x510
#define FWCFG_DATA  0x511
#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIR  0x0019

void fwcfg_read(u16 select, void *buf, u32 len)
{
    u8 *p = buf;
    outw(FWCFG_SEL, select);
    while (len--)
        *p++ = inb(FWCFG_DATA);
}

int fwcfg_present(void)
{
    char sig[4];
    fwcfg_read(FWCFG_SIGNATURE, sig, 4);
    return memcmp(sig, "QEMU", 4) == 0;
}

int fwcfg_find(const char *name, u16 *select, u32 *size)
{
    u8 count[4], entry[64];
    if (!fwcfg_present())
        return 0;
    outw(FWCFG_SEL, FWCFG_FILE_DIR);
    for (int i = 0; i < 4; i++)
        count[i] = inb(FWCFG_DATA);
    for (u32 n = be32(count); n; n--) {
        for (int i = 0; i < 64; i++)
            entry[i] = inb(FWCFG_DATA);
        entry[63] = 0;
        if (strcmp((const char *)entry + 8, name) == 0) {
            *size = be32(entry);
            *select = be16(entry + 4);
            return 1;
        }
    }
    return 0;
}
