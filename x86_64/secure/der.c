/* SquirrelBIOS x86_64-uefi: DER for X.509 public keys and Authenticode.
 * Original code, written from X.690 (DER), RFC 5280 (X.509), RFC 2315
 * (PKCS #7 SignedData) and Microsoft's published Authenticode format.
 *
 * Only what verification needs: the RSA key in a certificate, and in an
 * Authenticode SignedData the image digest from SpcIndirectDataContent,
 * the authenticated attributes (with the messageDigest of that content
 * checked here) and the first signer's signature. Certificate chains
 * are not walked: a signature counts when a key in db verifies it. */
#include "secure.h"

/* One TLV; 0 at the end or on a malformed length */
int der_next(struct der *d, struct tlv *t)
{
    const u8 *p = d->p;
    if (d->end - p < 2)
        return 0;
    t->raw = p;
    t->tag = *p++;
    u32 len = *p++;
    if (len & 0x80) {
        int n = len & 0x7F;
        if (n == 0 || n > 4 || d->end - p < n)
            return 0;
        len = 0;
        while (n--)
            len = len << 8 | *p++;
    }
    if ((u64)(d->end - p) < len)
        return 0;
    t->v = p;
    t->len = len;
    t->raw_len = (u32)(p + len - t->raw);
    d->p = p + len;
    return 1;
}

static struct der inside(const struct tlv *t)
{
    struct der d = { t->v, t->v + t->len };
    return d;
}

/* Next TLV, which must carry tag */
static int want(struct der *d, struct tlv *t, u8 tag)
{
    return der_next(d, t) && t->tag == tag;
}

static int oid_is(const struct tlv *t, const u8 *oid, u32 len)
{
    return t->tag == 0x06 && t->len == len && !memcmp(t->v, oid, len);
}

static const u8 OID_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const u8 OID_SIGNED_DATA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
static const u8 OID_MESSAGE_DIGEST[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x04 };
static const u8 OID_SPC_INDIRECT[] = { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 };
static const u8 OID_SHA256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };

/* An INTEGER as big-endian bytes without the sign byte */
static void unsigned_int(struct tlv *t)
{
    while (t->len > 1 && t->v[0] == 0)
        t->v++, t->len--;
}

/* SubjectPublicKeyInfo: SEQ { SEQ { rsaEncryption, NULL }, BIT STRING { SEQ { n, e } } } */
static int spki_rsa(const struct tlv *spki, struct rsa_key *k)
{
    struct der d = inside(spki), a, b, ne;
    struct tlv alg, oid, bits, seq, n, e;
    if (!want(&d, &alg, 0x30) || !want(&d, &bits, 0x03) || bits.len < 1 || bits.v[0] != 0)
        return 0;
    a = inside(&alg);
    if (!der_next(&a, &oid) || !oid_is(&oid, OID_RSA, sizeof OID_RSA))
        return 0;
    b.p = bits.v + 1;
    b.end = bits.v + bits.len;
    if (!want(&b, &seq, 0x30))
        return 0;
    ne = inside(&seq);
    if (!want(&ne, &n, 0x02) || !want(&ne, &e, 0x02) || e.len > 5)
        return 0;
    unsigned_int(&n);
    unsigned_int(&e);
    if (e.len > 4 || n.len > RSA_MAX_BYTES)
        return 0;
    u32 ev = 0;
    for (u32 i = 0; i < e.len; i++)
        ev = ev << 8 | e.v[i];
    k->n = n.v;
    k->n_len = n.len;
    k->e = ev;
    return 1;
}

/* Certificate: SEQ { tbsCertificate SEQ { [0] version?, serial, sigalg,
 * issuer, validity, subject, subjectPublicKeyInfo, ... }, ... } */
int x509_rsa_key(const u8 *cert, u32 len, struct rsa_key *k)
{
    struct der d = { cert, cert + len }, c, tbs;
    struct tlv t, tb, f;
    if (!want(&d, &t, 0x30))
        return 0;
    c = inside(&t);
    if (!want(&c, &tb, 0x30))
        return 0;
    tbs = inside(&tb);
    if (!der_next(&tbs, &f))
        return 0;
    if (f.tag == 0xA0 && !der_next(&tbs, &f))   /* version, then serial */
        return 0;
    for (int i = 0; i < 4; i++)                 /* sigalg, issuer, validity, subject */
        if (!der_next(&tbs, &f))
            return 0;
    return want(&tbs, &f, 0x30) && spki_rsa(&f, k);
}

/* The messageDigest attribute's value in the authenticated attributes */
static int attr_digest(const struct tlv *attrs, struct tlv *md)
{
    struct der d = inside(attrs);
    struct tlv a;
    while (der_next(&d, &a)) {
        struct der ad = inside(&a), sd;
        struct tlv oid, set;
        if (a.tag != 0x30 || !der_next(&ad, &oid) || !want(&ad, &set, 0x31))
            continue;
        if (!oid_is(&oid, OID_MESSAGE_DIGEST, sizeof OID_MESSAGE_DIGEST))
            continue;
        sd = inside(&set);
        return want(&sd, md, 0x04);
    }
    return 0;
}

/* ContentInfo { signedData, [0] SignedData { version, digestAlgorithms,
 *   ContentInfo { spcIndirectData, [0] SpcIndirectDataContent {
 *     SpcAttributeTypeAndOptionalValue, DigestInfo { algid, digest } } },
 *   [0] certificates?, [1] crls?, SET { SignerInfo { version,
 *   issuerAndSerialNumber, digestAlgorithm, [0] authenticatedAttributes?,
 *   digestEncryptionAlgorithm, encryptedDigest } } } } */
int pkcs7_authenticode(const u8 *p, u32 len, struct authenticode *a)
{
    struct der d = { p, p + len }, ci, sd, inner, spc, di, si, sig;
    struct tlv t, oid, x, content, spc_seq, dinfo, alg, digest, signers, signer, attrs;
    memset(a, 0, sizeof *a);
    if (!want(&d, &t, 0x30))
        return 0;
    ci = inside(&t);
    if (!der_next(&ci, &oid) || !oid_is(&oid, OID_SIGNED_DATA, sizeof OID_SIGNED_DATA) ||
        !want(&ci, &x, 0xA0))
        return 0;
    ci = inside(&x);
    if (!want(&ci, &t, 0x30))
        return 0;
    sd = inside(&t);
    if (!want(&sd, &x, 0x02) || !want(&sd, &x, 0x31) || !want(&sd, &content, 0x30))
        return 0;

    /* The signed content: SpcIndirectDataContent and the image digest in it */
    inner = inside(&content);
    if (!der_next(&inner, &oid) || !oid_is(&oid, OID_SPC_INDIRECT, sizeof OID_SPC_INDIRECT) ||
        !want(&inner, &x, 0xA0))
        return 0;
    spc = inside(&x);
    if (!want(&spc, &spc_seq, 0x30))
        return 0;
    spc = inside(&spc_seq);
    if (!want(&spc, &x, 0x30) || !want(&spc, &dinfo, 0x30))
        return 0;
    di = inside(&dinfo);
    if (!want(&di, &alg, 0x30) || !want(&di, &digest, 0x04))
        return 0;
    struct der ad = inside(&alg);
    if (!der_next(&ad, &oid) || !oid_is(&oid, OID_SHA256, sizeof OID_SHA256) || digest.len != 32)
        return 0;
    a->digest = digest.v;
    a->digest_len = digest.len;

    /* Skip certificates and CRLs, take the first signer */
    do {
        if (!der_next(&sd, &signers))
            return 0;
    } while (signers.tag == 0xA0 || signers.tag == 0xA1);
    if (signers.tag != 0x31)
        return 0;
    si = inside(&signers);
    if (!want(&si, &signer, 0x30))
        return 0;
    si = inside(&signer);
    if (!want(&si, &x, 0x02) || !want(&si, &x, 0x30) || !want(&si, &alg, 0x30))
        return 0;
    ad = inside(&alg);
    if (!der_next(&ad, &oid) || !oid_is(&oid, OID_SHA256, sizeof OID_SHA256))
        return 0;
    if (!der_next(&si, &attrs))
        return 0;
    if (attrs.tag == 0xA0) {
        /* The attributes are signed, encoded as a SET; their
         * messageDigest covers the content octets of the SpcIndirectData
         * SEQUENCE (its value, without tag and length) */
        struct tlv md;
        u8 h[32];
        if (!attr_digest(&attrs, &md) || md.len != 32)
            return 0;
        sha256(spc_seq.v, spc_seq.len, h);
        if (memcmp(h, md.v, 32))
            return 0;
        a->signed_data = attrs.raw;
        a->signed_len = attrs.raw_len;
        a->attrs_set = 1;
        if (!want(&si, &alg, 0x30))
            return 0;
    } else if (attrs.tag == 0x30) {
        a->signed_data = spc_seq.v;     /* no attributes: the content itself */
        a->signed_len = spc_seq.len;
    } else {
        return 0;
    }
    sig = si;
    if (!want(&sig, &x, 0x04))
        return 0;
    a->sig = x.v;
    a->sig_len = x.len;
    return 1;
}

/* A detached SignedData, as in EFI_VARIABLE_AUTHENTICATION_2 (the
 * SignedData alone, or in its ContentInfo): the first signer must sign
 * content_hash, directly or through authenticated attributes whose
 * messageDigest it is. Gives the signature and what it covers. */
int pkcs7_detached(const u8 *p, u32 len, const u8 content_hash[32], struct authenticode *a)
{
    struct der d = { p, p + len }, sd, si, ad, peek;
    struct tlv t, oid, x, signers, signer, alg, attrs;
    memset(a, 0, sizeof *a);
    if (!want(&d, &t, 0x30))
        return 0;
    sd = inside(&t);
    peek = sd;
    if (der_next(&peek, &oid) && oid.tag == 0x06) {         /* ContentInfo */
        if (!oid_is(&oid, OID_SIGNED_DATA, sizeof OID_SIGNED_DATA) || !want(&peek, &x, 0xA0))
            return 0;
        sd = inside(&x);
        if (!want(&sd, &t, 0x30))
            return 0;
        sd = inside(&t);
    }
    /* version, digestAlgorithms, the (empty) content */
    if (!want(&sd, &x, 0x02) || !want(&sd, &x, 0x31) || !want(&sd, &x, 0x30))
        return 0;
    do {
        if (!der_next(&sd, &signers))
            return 0;
    } while (signers.tag == 0xA0 || signers.tag == 0xA1);
    if (signers.tag != 0x31)
        return 0;
    si = inside(&signers);
    if (!want(&si, &signer, 0x30))
        return 0;
    si = inside(&signer);
    /* version, issuerAndSerialNumber or [0] subjectKeyIdentifier */
    if (!want(&si, &x, 0x02) || !der_next(&si, &x) || !want(&si, &alg, 0x30))
        return 0;
    ad = inside(&alg);
    if (!der_next(&ad, &oid) || !oid_is(&oid, OID_SHA256, sizeof OID_SHA256))
        return 0;
    if (!der_next(&si, &attrs))
        return 0;
    if (attrs.tag == 0xA0) {
        struct tlv md;
        if (!attr_digest(&attrs, &md) || md.len != 32 || memcmp(md.v, content_hash, 32))
            return 0;
        a->signed_data = attrs.raw;
        a->signed_len = attrs.raw_len;
        a->attrs_set = 1;
        if (!want(&si, &alg, 0x30))
            return 0;
    } else if (attrs.tag != 0x30) {
        return 0;
    }
    if (!want(&si, &x, 0x04))
        return 0;
    a->digest = content_hash;
    a->digest_len = 32;
    a->sig = x.v;
    a->sig_len = x.len;
    return 1;
}

