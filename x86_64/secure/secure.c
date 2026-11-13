/* SquirrelBIOS x86_64-uefi: verify-only Secure Boot (roadmap C4).
 * Original code, written from the UEFI specification's description of
 * image verification and signature databases and the published
 * Authenticode PE hash rules.
 *
 * Keys are ours, given to QEMU as fw_cfg files: opt/org.squirrelworks/db
 * (allowed) and opt/org.squirrelworks/dbx (forbidden), each a list of
 * EFI_SIGNATURE_LISTs with SHA-256 hashes, RSA-2048 keys or X.509
 * certificates (their RSA key is used). With a db, Secure Boot is on:
 * LoadImage refuses (EFI_SECURITY_VIOLATION) every image whose
 * Authenticode hash is in dbx or signed by a dbx key, and accepts one
 * whose PKCS#7 signature a db key verifies or whose hash is in db.
 * Nothing is enrolled at run time: SetVariable cannot change the
 * databases the firmware checks against (verify-only, no PK/KEK
 * authenticated writes). */
#include "secure.h"

#define FWCFG_DB  "opt/org.squirrelworks/db"
#define FWCFG_DBX "opt/org.squirrelworks/dbx"
#define DB_MAX    0x10000

#ifndef CONFIG_SECURE_BOOT
#define CONFIG_SECURE_BOOT 1
#endif

static const EFI_GUID cert_sha256 = { 0xc1c41626, 0x504c, 0x4092, { 0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28 } };
static const EFI_GUID cert_rsa2048 = { 0x3c5766e8, 0x269c, 0x4e34, { 0xaa, 0x14, 0xed, 0x77, 0x6e, 0x85, 0xb3, 0xb6 } };
static const EFI_GUID cert_x509 = { 0xa5c059a1, 0x94e4, 0x4aa7, { 0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72 } };
static const EFI_GUID image_security = { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } };
static const EFI_GUID global_var = { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };

struct sigdb {
    u8 *data;
    u32 len;
};

static struct sigdb db, dbx;
static int on;
static int broken;                     /* a db or dbx was given but cannot be read */

int secure_boot_on(void) { return on; }

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }

/* Walk a database: calls fn(type, data, size, ctx) for every signature
 * until fn returns nonzero; returns that, or -1 when the lists are
 * malformed, 0 at the end */
typedef int (*sig_fn)(const EFI_GUID *type, const u8 *data, u32 size, void *ctx);

static int db_walk(const struct sigdb *d, sig_fn fn, void *ctx)
{
    u32 off = 0;
    while (off < d->len) {
        if (d->len - off < 28)
            return -1;
        const u8 *l = d->data + off;
        u32 list = rd32(l + 16), hdr = rd32(l + 20), sz = rd32(l + 24);
        if (list < 28 || list > d->len - off || sz <= 16 || hdr > list - 28 ||
            (list - 28 - hdr) % sz)
            return -1;
        for (u32 at = 28 + hdr; at < list; at += sz) {
            int r = fn((const EFI_GUID *)l, l + at + 16, sz - 16, ctx);
            if (r)
                return r;
        }
        off += list;
    }
    return 0;
}

static int guid_is(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }

/* The key in an RSA-2048 or X.509 entry */
static int entry_key(const EFI_GUID *type, const u8 *data, u32 size, struct rsa_key *k)
{
    if (guid_is(type, &cert_rsa2048) && size == 256) {
        k->n = data;
        k->n_len = 256;
        k->e = 65537;
        return 1;
    }
    if (guid_is(type, &cert_x509))
        return x509_rsa_key(data, size, k);
    return 0;
}

static int has_hash(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    return guid_is(type, &cert_sha256) && size == 32 && !memcmp(data, ctx, 32);
}

struct sig_check {
    const u8 *hash;             /* of what was signed */
    const struct authenticode *a;
    int index;                  /* of the entry that verified */
    const char *kind;
};

static int verifies(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct sig_check *c = ctx;
    struct rsa_key k;
    c->index++;
    if (!entry_key(type, data, size, &k))
        return 0;
    if (!rsa_verify_sha256(&k, c->hash, c->a->sig, c->a->sig_len))
        return 0;
    c->kind = guid_is(type, &cert_x509) ? "X.509" : "RSA-2048";
    return 1;
}

struct count {
    int sha, rsa, x509, other;
};

static int count_entry(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct count *c = ctx;
    struct rsa_key k;
    if (guid_is(type, &cert_sha256))
        c->sha++;
    else if (entry_key(type, data, size, &k))
        guid_is(type, &cert_x509) ? c->x509++ : c->rsa++;
    else
        c->other++;
    return 0;
}

/* Authenticode PE hash: the headers without the checksum and the
 * certificate table entry, the sections in file order, then whatever
 * follows them except the certificate table */
int pe_authenticode_hash(const u8 *img, UINTN size, u8 out[32])
{
    if (size < 0x40 || img[0] != 'M' || img[1] != 'Z')
        return 0;
    u32 pe = rd32(img + 0x3C);
    if ((u64)pe + 24 + 112 > size || memcmp(img + pe, "PE\0\0", 4))
        return 0;
    const u8 *coff = img + pe + 4;
    u16 nsect = rd16(coff + 2), optsize = rd16(coff + 16);
    const u8 *opt = coff + 20;
    if (rd16(opt) != 0x20B || optsize < 112)
        return 0;
    u32 hdr = rd32(opt + 60), ndirs = rd32(opt + 108);
    const u8 *sect = opt + optsize;
    u32 cksum = (u32)(opt + 64 - img);
    u32 secdir = (u32)(opt + 112 + 4 * 8 - img);
    u32 cert_size = 0;
    if (ndirs > 4 && optsize >= 112 + 5 * 8)
        cert_size = rd32(img + secdir + 4);
    if (hdr > size || hdr < secdir + 8 || (u64)(sect - img) + nsect * 40ULL > size || nsect > 96)
        return 0;

    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, img, cksum);
    if (ndirs > 4) {
        sha256_update(&c, img + cksum + 4, secdir - cksum - 4);
        sha256_update(&c, img + secdir + 8, hdr - secdir - 8);
    } else {
        sha256_update(&c, img + cksum + 4, hdr - cksum - 4);
    }

    const u8 *order[96];
    for (u16 i = 0; i < nsect; i++) {
        const u8 *s = sect + i * 40;
        int j = i;
        for (; j > 0 && rd32(order[j - 1] + 20) > rd32(s + 20); j--)
            order[j] = order[j - 1];
        order[j] = s;
    }
    u64 done = hdr;
    for (u16 i = 0; i < nsect; i++) {
        u32 raw = rd32(order[i] + 16), ptr = rd32(order[i] + 20);
        if (!raw)
            continue;
        if ((u64)ptr + raw > size)
            return 0;
        sha256_update(&c, img + ptr, raw);
        done += raw;
    }
    if (size > done + cert_size)
        sha256_update(&c, img + done, size - done - cert_size);
    sha256_final(&c, out);
    return 1;
}

static void say_refused(const char *why)
{
    printf("secure: image refused: %s\n", why);
}

EFI_STATUS secure_check_image(const u8 *img, UINTN size)
{
    if (!on)
        return EFI_SUCCESS;
    if (broken) {
        say_refused("the db or dbx given could not be read, so nothing is trusted");
        return EFI_SECURITY_VIOLATION;
    }
    u8 h[32];
    if (!pe_authenticode_hash(img, size, h)) {
        say_refused("not a PE32+ image it can hash");
        return EFI_SECURITY_VIOLATION;
    }
    if (db_walk(&dbx, has_hash, h) > 0) {
        say_refused("its SHA-256 is in dbx");
        return EFI_SECURITY_VIOLATION;
    }

    /* The certificate table: WIN_CERTIFICATEs, 8-byte aligned */
    u32 pe = rd32(img + 0x3C);
    const u8 *opt = img + pe + 24;
    u32 ndirs = rd32(opt + 108);
    u32 off = 0, len = 0;
    if (ndirs > 4) {
        off = rd32(opt + 112 + 4 * 8);
        len = rd32(opt + 112 + 4 * 8 + 4);
    }
    const char *why = "not signed, and its SHA-256 is not in db";
    if (len && ((u64)off + len > size || off < 0x40))
        len = 0, why = "its certificate table is out of the file";
    for (u32 at = 0; len && at + 8 <= len;) {
        const u8 *w = img + off + at;
        u32 wlen = rd32(w);
        if (wlen < 8 || wlen > len - at)
            break;
        if (rd16(w + 4) == 0x0200 && rd16(w + 6) == 0x0002) {   /* PKCS_SIGNED_DATA */
            struct authenticode a;
            if (!pkcs7_authenticode(w + 8, wlen - 8, &a)) {
                why = "a signature it cannot read";
            } else if (memcmp(a.digest, h, 32)) {
                why = "a signature for another image (hash mismatch)";
            } else {
                u8 sh[32];
                struct sha256 c;
                sha256_init(&c);
                if (a.attrs_set) {
                    u8 set = 0x31;
                    sha256_update(&c, &set, 1);
                    sha256_update(&c, a.signed_data + 1, a.signed_len - 1);
                } else {
                    sha256_update(&c, a.signed_data, a.signed_len);
                }
                sha256_final(&c, sh);
                struct sig_check chk = { sh, &a, 0, NULL };
                if (db_walk(&dbx, verifies, &chk) > 0) {
                    say_refused("signed by a key in dbx");
                    return EFI_SECURITY_VIOLATION;
                }
                chk.index = 0;
                if (db_walk(&db, verifies, &chk) > 0) {
                    printf("secure: image verified: signed by db entry %d (%s)\n", chk.index, chk.kind);
                    return EFI_SUCCESS;
                }
                why = "signed by a key that is not in db";
            }
        }
        at += (wlen + 7) & ~7u;
    }
    if (db_walk(&db, has_hash, h) > 0) {
        printf("secure: image verified: its SHA-256 is in db\n");
        return EFI_SUCCESS;
    }
    say_refused(why);
    return EFI_SECURITY_VIOLATION;
}

static int load_db(const char *name, struct sigdb *d)
{
    u16 sel;
    u32 size;
    if (!fwcfg_find(name, &sel, &size) || !size)
        return 0;
    if (size > DB_MAX || efi_allocate_pool(EfiBootServicesData, size, (void **)&d->data) != EFI_SUCCESS) {
        printf("secure: %s too big (%u bytes)\n", name, size);
        return -1;
    }
    fwcfg_read(sel, d->data, size);
    d->len = size;
    if (db_walk(d, count_entry, &(struct count){ 0 }) < 0) {
        printf("secure: %s is not a list of EFI_SIGNATURE_LISTs\n", name);
        d->len = 0;
        return -1;
    }
    return 1;
}

static void describe(const char *name, const struct sigdb *d)
{
    struct count c = { 0 };
    db_walk(d, count_entry, &c);
    printf("secure: %s: %d SHA-256, %d RSA-2048, %d X.509", name, c.sha, c.rsa, c.x509);
    if (c.other)
        printf(", %d not usable", c.other);
    printf("\n");
}

void secure_init(void)
{
    if (!CONFIG_SECURE_BOOT)
        return;
    int r = load_db(FWCFG_DB, &db);
    if (r == 0)
        return;                         /* no db: Secure Boot off */
    int rx = load_db(FWCFG_DBX, &dbx);
    on = 1;
    /* Fail closed: keys were meant to be enforced, so a db or dbx that
     * does not parse refuses every image rather than allowing all */
    broken = r < 0 || rx < 0;
    printf("secure: Secure Boot on (verify-only)%s\n",
           broken ? ", but a key database is unreadable: every image is refused" : "");
    describe("db", &db);
    describe("dbx", &dbx);

    /* What an OS reads: SecureBoot = 1, SetupMode = 0, and db/dbx when
     * the variable store has room for them */
    typedef EFI_STATUS (EFIAPI *set_var_fn)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    set_var_fn set = (set_var_fn)efi_st->runtime_services->set_variable;
    u8 one = 1, zero = 0;
    EFI_STATUS st;
    if ((st = set(u"SecureBoot", (EFI_GUID *)&global_var, 6, 1, &one)) != EFI_SUCCESS)
        printf("secure: SecureBoot variable not set (0x%lx): the OS will not see Secure Boot on\n", st);
    if ((st = set(u"SetupMode", (EFI_GUID *)&global_var, 6, 1, &zero)) != EFI_SUCCESS)
        printf("secure: SetupMode variable not set (0x%lx)\n", st);
    if (db.len && set(u"db", (EFI_GUID *)&image_security, 7, db.len, db.data) != EFI_SUCCESS)
        printf("secure: db (%u bytes) not published as a variable\n", db.len);
    if (dbx.len && set(u"dbx", (EFI_GUID *)&image_security, 7, dbx.len, dbx.data) != EFI_SUCCESS)
        printf("secure: dbx (%u bytes) not published as a variable\n", dbx.len);
}
