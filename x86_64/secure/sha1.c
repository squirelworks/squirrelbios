/* SquirrelBIOS x86_64-uefi: SHA-1, for a TPM's SHA-1 PCR bank only.
 * Original code, written from FIPS 180-4. */
#include "secure.h"

static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void block(struct sha1 *c, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)
            f = (b & cc) | (~b & d), k = 0x5a827999;
        else if (i < 40)
            f = b ^ cc ^ d, k = 0x6ed9eba1;
        else if (i < 60)
            f = (b & cc) | (b & d) | (cc & d), k = 0x8f1bbcdc;
        else
            f = b ^ cc ^ d, k = 0xca62c1d6;
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = rol(b, 30), b = a, a = t;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e;
}

void sha1_init(struct sha1 *c)
{
    static const u32 iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->fill = 0;
}

void sha1_update(struct sha1 *c, const void *data, usize n)
{
    const u8 *p = data;
    c->len += n;
    while (n) {
        if (!c->fill && n >= 64) {
            block(c, p);
            p += 64, n -= 64;
            continue;
        }
        u32 take = 64 - c->fill;
        if (take > n)
            take = (u32)n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take, p += take, n -= take;
        if (c->fill == 64) {
            block(c, c->buf);
            c->fill = 0;
        }
    }
}

void sha1_final(struct sha1 *c, u8 out[20])
{
    u64 bits = c->len * 8;
    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56)
        sha1_update(c, &pad, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (u8)(c->h[i] >> (24 - 8 * j));
}
