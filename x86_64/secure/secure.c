/* SquirrelBIOS x86_64-uefi: Secure Boot (roadmap C4/C5).
 * Original code, written from the UEFI specification's description of
 * image verification, signature databases and the setup and user modes,
 * and the published Authenticode PE hash rules.
 *
 * The keys come from one of two places.
 *
 * Fixed keys: QEMU fw_cfg files opt/org.squirrelworks/db (allowed) and
 * opt/org.squirrelworks/dbx (forbidden), lists of EFI_SIGNATURE_LISTs
 * with SHA-256 hashes, RSA-2048 keys or X.509 certificates (their RSA key
 * is used). With a db, Secure Boot is on and nothing can be enrolled.
 *
 * Enrolled keys: otherwise the PK, KEK, db and dbx variables, written
 * with authenticated writes (authvar.c) and kept in the flash store when
 * there is one. With a PK the machine is in user mode and Secure Boot is
 * on; without one it is in setup mode and every image runs. At boot,
 * fw_cfg files opt/org.squirrelworks/enroll/{db,dbx,KEK,PK} (each an
 * EFI_VARIABLE_AUTHENTICATION_2 followed by the data, as an OS tool would
 * pass to SetVariable) are written in that order, under the same rules.
 *
 * When on, LoadImage refuses (EFI_SECURITY_VIOLATION) every image whose
 * Authenticode hash is in dbx or signed by a dbx key, and accepts one
 * whose PKCS#7 signature a db key verifies or whose hash is in db. */
#include "secure.h"

#define FWCFG_DB  "opt/org.squirrelworks/db"
#define FWCFG_DBX "opt/org.squirrelworks/dbx"
#define DB_MAX    0x10000

#ifndef CONFIG_SECURE_BOOT
#define CONFIG_SECURE_BOOT 1
#endif

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

static int db_walk(const struct sigdb *d, sb_sig_fn fn, void *ctx)
{
    return sb_db_walk(d->data, d->len, fn, ctx);
}

static int guid_is(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }
#define entry_key sb_entry_key
#define cert_sha256 sb_cert_sha256
#define cert_x509 sb_cert_x509
#define image_security sb_image_security
#define global_var sb_global

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
                sb_signed_hash(&a, sh);
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

/* A variable into a buffer of its own: 1, 0 when missing, -1 when it
 * cannot be read */
static int load_var(const CHAR16 *name, const EFI_GUID *vendor, struct sigdb *d)
{
    UINTN size = 0;
    EFI_STATUS st = vars_get((CHAR16 *)name, (EFI_GUID *)vendor, NULL, &size, NULL);
    if (st == EFI_NOT_FOUND)
        return 0;
    if (st != EFI_BUFFER_TOO_SMALL || !size || size > DB_MAX ||
        efi_allocate_pool(EfiBootServicesData, size, (void **)&d->data) != EFI_SUCCESS ||
        vars_get((CHAR16 *)name, (EFI_GUID *)vendor, NULL, &size, d->data) != EFI_SUCCESS)
        return -1;
    d->len = (u32)size;
    return db_walk(d, count_entry, &(struct count){ 0 }) < 0 ? -1 : 1;
}

static const char *status_text(EFI_STATUS st)
{
    if (st == EFI_SUCCESS)
        return "enrolled";
    if (st == EFI_SECURITY_VIOLATION)
        return "refused: not signed by a key that may change it, or not newer";
    if (st == EFI_WRITE_PROTECTED)
        return "refused: the keys are fixed";
    if (st == EFI_INVALID_PARAMETER)
        return "refused: not a signature list";
    return "refused";
}

/* fw_cfg enroll/<name>: an authenticated write, as an OS would make */
static void enroll(const char *file, const CHAR16 *name, const EFI_GUID *vendor)
{
    u16 sel;
    u32 size;
    char path[48] = "opt/org.squirrelworks/enroll/";
    u32 n = 0;
    while (path[n])
        n++;
    for (u32 i = 0; file[i] && n < sizeof path - 1; i++)
        path[n++] = file[i];
    path[n] = 0;
    if (!fwcfg_find(path, &sel, &size) || !size)
        return;
    u8 *buf;
    if (size > DB_MAX || efi_allocate_pool(EfiBootServicesData, size, (void **)&buf) != EFI_SUCCESS) {
        printf("secure: enroll %s: too big\n", file);
        return;
    }
    fwcfg_read(sel, buf, size);
    EFI_STATUS st = vars_set((CHAR16 *)name, (EFI_GUID *)vendor, 0x27, size, buf);
    printf("secure: enroll %s: %s\n", file, status_text(st));
    efi_free_pool(buf);
}

void secure_init(void)
{
    if (!CONFIG_SECURE_BOOT)
        return;
    int r = load_db(FWCFG_DB, &db);
    if (r != 0) {
        /* Fixed keys */
        int rx = load_db(FWCFG_DBX, &dbx);
        authvar_init(1);
        on = 1;
        /* Fail closed: keys were meant to be enforced, so a db or dbx that
         * does not parse refuses every image rather than allowing all */
        broken = r < 0 || rx < 0;
        printf("secure: Secure Boot on (fixed keys)%s\n",
               broken ? ", but a key database is unreadable: every image is refused" : "");
        describe("db", &db);
        describe("dbx", &dbx);
        authvar_set_mode(1, 0);
        if (db.len && authvar_publish(u"db", &image_security, db.data, db.len) != EFI_SUCCESS)
            printf("secure: db (%u bytes) not published as a variable\n", db.len);
        if (dbx.len && authvar_publish(u"dbx", &image_security, dbx.data, dbx.len) != EFI_SUCCESS)
            printf("secure: dbx (%u bytes) not published as a variable\n", dbx.len);
        return;
    }

    /* Enrolled keys */
    authvar_init(0);
    authvar_set_mode(0, sb_setup_mode());
    enroll("db", u"db", &image_security);
    enroll("dbx", u"dbx", &image_security);
    enroll("KEK", u"KEK", &global_var);
    enroll("PK", u"PK", &global_var);
    if (sb_setup_mode()) {
        printf("secure: setup mode (no PK): Secure Boot off\n");
        authvar_set_mode(0, 1);
        return;
    }
    int rd = load_var(u"db", &image_security, &db);
    int rx = load_var(u"dbx", &image_security, &dbx);
    on = 1;
    broken = rd < 0 || rx < 0;
    struct sigdb kek = { 0 };
    load_var(u"KEK", &global_var, &kek);
    struct count kc = { 0 };
    db_walk(&kek, count_entry, &kc);
    printf("secure: Secure Boot on (user mode: PK and %d KEK key%s enrolled)%s\n", kc.rsa + kc.x509,
           kc.rsa + kc.x509 == 1 ? "" : "s", broken ? ", but db or dbx is unreadable: every image is refused" : "");
    describe("db", &db);
    describe("dbx", &dbx);
    authvar_set_mode(1, 0);
}
