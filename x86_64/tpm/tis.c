/* SquirrelBIOS x86_64-uefi: a TPM 2.0 behind the TIS/FIFO interface.
 * Original code, written from the TCG PC Client Platform TPM Profile
 * (the FIFO register interface at 0xFED40000, locality 0), as QEMU's
 * tpm-tis device presents it. */
#include "tpm.h"

#define ACCESS      0x00
#define STS         0x18
#define FIFO        0x24
#define DID_VID     0xF00

#define ACC_VALID   0x80
#define ACC_ACTIVE  0x20
#define ACC_REQUEST 0x02
#define STS_VALID   0x80
#define STS_READY   0x40
#define STS_GO      0x20
#define STS_AVAIL   0x10
#define STS_EXPECT  0x08

/* Through a register the compiler can't see the value of, so a fixed
 * MMIO address doesn't trip its object-size checks */
static volatile u8 *reg(u32 off)
{
    uptr p = TPM_TIS_BASE + off;
    __asm__("" : "+r"(p));
    return (volatile u8 *)p;
}

static u8 rd8(u32 off) { return *reg(off); }
static u32 rd32(u32 off) { return *(volatile u32 *)reg(off); }
static void wr8(u32 off, u8 v) { *reg(off) = v; }

static int wait(u32 off, u32 mask, u32 want, u32 ms)
{
    u64 end = arch_counter() + arch_counter_hz() / 1000 * ms;
    while ((rd32(off) & mask) != want)
        if (arch_counter() > end)
            return 0;
    return 1;
}

static u32 burst(void)
{
    return rd32(STS) >> 8 & 0xFFFF;
}

int tis_probe(u32 *did_vid)
{
    u32 id = rd32(DID_VID);
    *did_vid = id;
    if (!id || id == 0xFFFFFFFF || !(rd8(ACCESS) & ACC_VALID))
        return 0;
    return 1;
}

/* One command: in[] holds it whole (its size in the header); the
 * response goes to out[] (cap bytes). Returns the response size, or 0. */
u32 tis_command(const u8 *in, u32 len, u8 *out, u32 cap)
{
    u32 got = 0;
    wr8(ACCESS, ACC_REQUEST);
    if (!wait(ACCESS, ACC_ACTIVE | ACC_VALID, ACC_ACTIVE | ACC_VALID, 750))
        return 0;
    wr8(STS, STS_READY);
    if (!wait(STS, STS_READY, STS_READY, 750))
        goto done;
    u64 end = arch_counter() + arch_counter_hz() * 2;
    for (u32 i = 0; i < len;) {
        u32 n = burst();
        if (!n) {
            if (arch_counter() > end)
                goto done;
            continue;
        }
        while (n-- && i < len)
            wr8(FIFO, in[i++]);
    }
    if (!wait(STS, STS_VALID | STS_EXPECT, STS_VALID, 750))
        goto done;
    wr8(STS, STS_GO);
    /* Long enough for any command a TPM may take its time over */
    if (!wait(STS, STS_VALID | STS_AVAIL, STS_VALID | STS_AVAIL, 120000))
        goto done;
    u32 want = 10;
    while (got < want) {
        if (!wait(STS, STS_VALID | STS_AVAIL, STS_VALID | STS_AVAIL, 750)) {
            got = 0;
            goto done;
        }
        u32 n = burst();
        while (n-- && got < want) {
            u8 b = rd8(FIFO);
            if (got < cap)
                out[got] = b;
            got++;
            if (got == 6) {
                want = be32(out + 2);
                if (want < 10 || want > cap) {
                    got = 0;
                    goto done;
                }
            }
        }
    }
done:
    wr8(STS, STS_READY);
    wr8(ACCESS, ACC_ACTIVE);            /* give up the locality */
    return got;
}
