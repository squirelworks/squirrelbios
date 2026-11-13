/* SquirrelBIOS x86_64-uefi: RSA signature verification.
 * Original code, written from RFC 8017 (RSASSA-PKCS1-v1_5) and the
 * textbook Montgomery multiplication. Public keys only: nothing here
 * needs to be constant-time. */
#include "secure.h"

#define LIMBS (RSA_MAX_BYTES / 4)

typedef struct {
    u32 l[LIMBS + 2];
} num;

/* big-endian bytes to little-endian limbs */
static void load(num *x, const u8 *p, u32 len, int L)
{
    memset(x, 0, sizeof *x);
    for (u32 i = 0; i < len; i++) {
        u32 bit = (len - 1 - i) * 8;
        x->l[bit / 32] |= (u32)p[i] << (bit % 32);
    }
    (void)L;
}

static void store(const num *x, u8 *p, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 bit = (len - 1 - i) * 8;
        p[i] = (u8)(x->l[bit / 32] >> (bit % 32));
    }
}

/* a >= b over L limbs (plus a's top limb) */
static int geq(const u32 *a, u32 top, const u32 *b, int L)
{
    if (top)
        return 1;
    for (int i = L - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 1;
}

static void sub(u32 *a, const u32 *b, int L)
{
    u64 borrow = 0;
    for (int i = 0; i < L; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        a[i] = (u32)d;
        borrow = (d >> 32) & 1;
    }
}

/* r = a * b / 2^(32L) mod n */
static void mont_mul(num *r, const num *a, const num *b, const num *n, u32 n0, int L)
{
    u32 t[LIMBS + 2];
    memset(t, 0, sizeof t);
    for (int i = 0; i < L; i++) {
        u64 c = 0;
        for (int j = 0; j < L; j++) {
            u64 v = (u64)t[j] + (u64)a->l[j] * b->l[i] + c;
            t[j] = (u32)v;
            c = v >> 32;
        }
        u64 v = (u64)t[L] + c;
        t[L] = (u32)v;
        t[L + 1] = (u32)(v >> 32);
        u32 m = t[0] * n0;
        c = ((u64)t[0] + (u64)m * n->l[0]) >> 32;
        for (int j = 1; j < L; j++) {
            v = (u64)t[j] + (u64)m * n->l[j] + c;
            t[j - 1] = (u32)v;
            c = v >> 32;
        }
        v = (u64)t[L] + c;
        t[L - 1] = (u32)v;
        t[L] = t[L + 1] + (u32)(v >> 32);
    }
    if (geq(t, t[L], n->l, L))
        sub(t, n->l, L);
    memset(r, 0, sizeof *r);
    memcpy(r->l, t, L * 4);
}

/* out = s^e mod n; 0 when the input is out of range */
static int modexp(u8 *out, const struct rsa_key *k, const u8 *sig)
{
    int L = (k->n_len + 3) / 4;
    static num n, s, r2, x, sm, one;
    load(&n, k->n, k->n_len, L);
    load(&s, sig, k->n_len, L);
    if (!(n.l[0] & 1) || geq(s.l, 0, n.l, L))
        return 0;
    /* -n^-1 mod 2^32 by Newton's iteration */
    u32 inv = 1;
    for (int i = 0; i < 5; i++)
        inv *= 2 - n.l[0] * inv;
    u32 n0 = -inv;
    /* R^2 mod n, R = 2^(32L): double 1 that many times */
    memset(&r2, 0, sizeof r2);
    r2.l[0] = 1;
    for (int i = 0; i < 64 * L; i++) {
        u32 carry = r2.l[L - 1] >> 31;
        for (int j = L - 1; j > 0; j--)
            r2.l[j] = r2.l[j] << 1 | r2.l[j - 1] >> 31;
        r2.l[0] <<= 1;
        if (geq(r2.l, carry, n.l, L))
            sub(r2.l, n.l, L);
    }
    mont_mul(&sm, &s, &r2, &n, n0, L);         /* s in Montgomery form */
    x = sm;
    int top = 31;
    while (top > 0 && !(k->e >> top & 1))
        top--;
    for (int b = top - 1; b >= 0; b--) {
        mont_mul(&x, &x, &x, &n, n0, L);
        if (k->e >> b & 1)
            mont_mul(&x, &x, &sm, &n, n0, L);
    }
    memset(&one, 0, sizeof one);
    one.l[0] = 1;
    mont_mul(&x, &x, &one, &n, n0, L);
    store(&x, out, k->n_len);
    return 1;
}

/* EM = 00 01 FF..FF 00 DigestInfo(SHA-256) hash */
int rsa_verify_sha256(const struct rsa_key *k, const u8 hash[32], const u8 *sig, u32 sig_len)
{
    static const u8 info[19] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
    };
    static u8 em[RSA_MAX_BYTES];
    u32 len = k->n_len;
    if (len < 64 || len > RSA_MAX_BYTES || sig_len != len || !(k->e & 1) || k->e < 3)
        return 0;
    if (!modexp(em, k, sig))
        return 0;
    u32 pad = len - 3 - sizeof info - 32;
    if (em[0] != 0 || em[1] != 1)
        return 0;
    for (u32 i = 0; i < pad; i++)
        if (em[2 + i] != 0xFF)
            return 0;
    return em[2 + pad] == 0 && !memcmp(em + 3 + pad, info, sizeof info) &&
           !memcmp(em + 3 + pad + sizeof info, hash, 32);
}
