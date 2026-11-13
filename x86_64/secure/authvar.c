/* SquirrelBIOS x86_64-uefi: authenticated variables for Secure Boot keys
 * (roadmap C4/C5). Original code, written from the UEFI specification
 * (8.2.1 time-based authenticated variables, 32.3 the PK, KEK, db and dbx
 * and the setup and user modes, 32.4 signature lists).
 *
 * Built into the runtime pages with der.c, rsa.c and sha256.c: the OS
 * may enroll keys after ExitBootServices. variables.c calls authvar_hook
 * for every write; it guards PK, KEK (EFI global variables), db and dbx
 * (image security database), and the firmware's read-only SetupMode and
 * SecureBoot.
 *
 * A write to PK, KEK, db or dbx carries EFI_VARIABLE_TIME_BASED_
 * AUTHENTICATED_WRITE_ACCESS and starts with EFI_VARIABLE_AUTHENTICATION_2:
 * an EFI_TIME, then a WIN_CERTIFICATE_UEFI_GUID holding a detached PKCS#7
 * SignedData over SHA-256(name without its NUL, vendor GUID, attributes,
 * the time, the new data). The time must be later than the stored one
 * (an append keeps the later of the two). In user mode (a PK is set) the
 * signer must be the PK for PK and KEK, a KEK or the PK for db and dbx;
 * the key must be in that list itself (no chains). In setup mode (no PK)
 * the descriptor is checked but no signer is needed. The data must be a
 * well-formed list of EFI_SIGNATURE_LISTs; a PK holds one key. Writing
 * the PK enters user mode and deleting it (an empty, PK-signed write)
 * returns to setup mode: SetupMode follows at once, SecureBoot at the next
 * boot. With keys fixed by fw_cfg (opt/org.squirrelworks/db) nothing can
 * be enrolled. Private authenticated variables are not supported. */
#include "secure.h"

const EFI_GUID sb_cert_sha256 = { 0xc1c41626, 0x504c, 0x4092, { 0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28 } };
const EFI_GUID sb_cert_rsa2048 = { 0x3c5766e8, 0x269c, 0x4e34, { 0xaa, 0x14, 0xed, 0x77, 0x6e, 0x85, 0xb3, 0xb6 } };
const EFI_GUID sb_cert_x509 = { 0xa5c059a1, 0x94e4, 0x4aa7, { 0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72 } };
const EFI_GUID sb_image_security = { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } };
const EFI_GUID sb_global = { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };
static const EFI_GUID cert_pkcs7 = { 0x4aafd29d, 0x68df, 0x49ee, { 0x8a, 0xa9, 0x34, 0x7d, 0x37, 0x56, 0x65, 0xa7 } };

#define ATTR_NV     0x01
#define ATTR_BS     0x02
#define ATTR_RT     0x04
#define ATTR_TIME   0x20
#define ATTR_APPEND 0x40
#define KEY_ATTRS   (ATTR_NV | ATTR_BS | ATTR_RT | ATTR_TIME)

static int fixed;                       /* keys from fw_cfg: no enrollment */
static int internal;                    /* the firmware sets its own variables */
static u8 pk_buf[2048];                 /* PK or KEK, read for a check */
static u8 kek_buf[16384];

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }
static int guid_is(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }

int sb_db_walk(const u8 *data, u32 len, sb_sig_fn fn, void *ctx)
{
    u32 off = 0;
    while (off < len) {
        if (len - off < 28)
            return -1;
        const u8 *l = data + off;
        u32 list = rd32(l + 16), hdr = rd32(l + 20), sz = rd32(l + 24);
        if (list < 28 || list > len - off || sz <= 16 || hdr > list - 28 || (list - 28 - hdr) % sz)
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

int sb_entry_key(const EFI_GUID *type, const u8 *data, u32 size, struct rsa_key *k)
{
    if (guid_is(type, &sb_cert_rsa2048) && size == 256) {
        k->n = data;
        k->n_len = 256;
        k->e = 65537;
        return 1;
    }
    if (guid_is(type, &sb_cert_x509))
        return x509_rsa_key(data, size, k);
    return 0;
}

/* What the signature covers: the attributes as a SET, or the content */
void sb_signed_hash(const struct authenticode *a, u8 out[32])
{
    struct sha256 c;
    sha256_init(&c);
    if (a->attrs_set) {
        u8 set = 0x31;
        sha256_update(&c, &set, 1);
        sha256_update(&c, a->signed_data + 1, a->signed_len - 1);
        sha256_final(&c, out);
    } else if (a->signed_data) {
        sha256(a->signed_data, a->signed_len, out);
    } else {
        memcpy(out, a->digest, 32);
    }
}

struct check {
    const u8 *hash;
    const struct authenticode *a;
};

static int verifies(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct check *c = ctx;
    struct rsa_key k;
    return sb_entry_key(type, data, size, &k) && rsa_verify_sha256(&k, c->hash, c->a->sig, c->a->sig_len);
}

static int count_keys(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct rsa_key k;
    if (sb_entry_key(type, data, size, &k))
        ++*(int *)ctx;
    else
        *(int *)ctx += 1000;            /* not a key */
    return 0;
}

static int name_is(const CHAR16 *n, const char *s)
{
    while (*s)
        if (*n++ != (CHAR16)*s++)
            return 0;
    return *n == 0;
}

enum { V_OTHER, V_PK, V_KEK, V_DB, V_DBX, V_READONLY };

static int kind(const CHAR16 *name, const EFI_GUID *vendor)
{
    if (guid_is(vendor, &sb_global)) {
        if (name_is(name, "PK"))
            return V_PK;
        if (name_is(name, "KEK"))
            return V_KEK;
        if (name_is(name, "SetupMode") || name_is(name, "SecureBoot"))
            return V_READONLY;
    } else if (guid_is(vendor, &sb_image_security)) {
        if (name_is(name, "db"))
            return V_DB;
        if (name_is(name, "dbx"))
            return V_DBX;
    }
    return V_OTHER;
}

static int time_after(const EFI_TIME *a, const EFI_TIME *b)
{
    u32 x[7] = { a->year, a->month, a->day, a->hour, a->minute, a->second, a->nanosecond };
    u32 y[7] = { b->year, b->month, b->day, b->hour, b->minute, b->second, b->nanosecond };
    for (int i = 0; i < 7; i++)
        if (x[i] != y[i])
            return x[i] > y[i];
    return 0;
}

/* A key list from a variable: PK or KEK */
static u32 read_list(const char *which, u8 *buf, u32 cap)
{
    CHAR16 n[4] = { (CHAR16)which[0], (CHAR16)which[1], (CHAR16)which[2], 0 };
    UINTN size = cap;
    if (vars_get(n, (EFI_GUID *)&sb_global, NULL, &size, buf) != EFI_SUCCESS)
        return 0;
    return (u32)size;
}

int sb_setup_mode(void)
{
    UINTN size = sizeof pk_buf;
    return vars_get(u"PK", (EFI_GUID *)&sb_global, NULL, &size, pk_buf) != EFI_SUCCESS;
}

static EFI_STATUS hook(const CHAR16 *name, const EFI_GUID *vendor, u32 attr, const u8 **data,
                       u32 *size, EFI_TIME *time, const EFI_TIME *old, int exited)
{
    (void)exited;
    if (internal)
        return EFI_SUCCESS;
    int k = kind(name, vendor);
    if (k == V_READONLY)
        return EFI_WRITE_PROTECTED;
    if (k == V_OTHER)
        return (attr & ATTR_TIME) ? EFI_UNSUPPORTED : EFI_SUCCESS;
    if (fixed)
        return EFI_WRITE_PROTECTED;
    if ((attr & ~ATTR_APPEND) != KEY_ATTRS || (k == V_PK && (attr & ATTR_APPEND)))
        return EFI_SECURITY_VIOLATION;

    /* EFI_VARIABLE_AUTHENTICATION_2 */
    const u8 *p = *data;
    u32 n = *size;
    if (n < 16 + 24)
        return EFI_SECURITY_VIOLATION;
    EFI_TIME ts;
    memcpy(&ts, p, 16);
    u32 wlen = rd32(p + 16);
    if (wlen < 24 || wlen > n - 16 || rd16(p + 20) != 0x0200 || rd16(p + 22) != 0x0EF1 ||
        !guid_is((const EFI_GUID *)(p + 24), &cert_pkcs7))
        return EFI_SECURITY_VIOLATION;
    if (ts.pad1 || ts.nanosecond || ts.timezone || ts.daylight || ts.pad2)
        return EFI_SECURITY_VIOLATION;
    const u8 *cert = p + 40, *payload = p + 16 + wlen;
    u32 cert_len = wlen - 24, psize = n - 16 - wlen;
    if (old && !(attr & ATTR_APPEND) && !time_after(&ts, old))
        return EFI_SECURITY_VIOLATION;  /* a replay or an older write */

    /* The data: signature lists; one key in a PK */
    if (psize) {
        int keys = 0;
        if (sb_db_walk(payload, psize, count_keys, &keys) < 0)
            return EFI_INVALID_PARAMETER;
        if ((k == V_PK || k == V_KEK) && (keys == 0 || keys >= 1000))
            return EFI_INVALID_PARAMETER;
        if (k == V_PK && keys != 1)
            return EFI_INVALID_PARAMETER;
    }

    /* What was signed */
    u8 h[32];
    struct sha256 c;
    sha256_init(&c);
    u32 nl = 0;
    while (name[nl])
        nl++;
    sha256_update(&c, name, nl * 2);
    sha256_update(&c, vendor, 16);
    sha256_update(&c, &attr, 4);
    sha256_update(&c, &ts, 16);
    sha256_update(&c, payload, psize);
    sha256_final(&c, h);
    struct authenticode a;
    if (!pkcs7_detached(cert, cert_len, h, &a))
        return EFI_SECURITY_VIOLATION;

    if (!sb_setup_mode()) {
        u8 sh[32];
        sb_signed_hash(&a, sh);
        struct check chk = { sh, &a };
        u32 pk_len = read_list("PK", pk_buf, sizeof pk_buf);
        int ok = pk_len && sb_db_walk(pk_buf, pk_len, verifies, &chk) > 0;
        if (!ok && (k == V_DB || k == V_DBX)) {
            u32 kek_len = read_list("KEK", kek_buf, sizeof kek_buf);
            ok = kek_len && sb_db_walk(kek_buf, kek_len, verifies, &chk) > 0;
        }
        if (!ok)
            return EFI_SECURITY_VIOLATION;
    }
    *data = payload;
    *size = psize;
    if (!old || time_after(&ts, old))
        *time = ts;
    return EFI_SUCCESS;
}

static void set_internal(const CHAR16 *name, const EFI_GUID *vendor, u32 attr, UINTN size, const void *data)
{
    internal = 1;
    vars_set((CHAR16 *)name, (EFI_GUID *)vendor, attr, size, (void *)data);
    internal = 0;
}

/* After a write that went through: the PK moves SetupMode */
static void after(const CHAR16 *name, const EFI_GUID *vendor)
{
    if (kind(name, vendor) == V_PK) {
        u8 setup = (u8)sb_setup_mode();
        set_internal(u"SetupMode", &sb_global, ATTR_BS | ATTR_RT, 1, &setup);
    }
}

void authvar_set_mode(int secure_boot, int setup)          /* boot time */
{
    u8 sb = (u8)secure_boot, sm = (u8)setup;
    set_internal(u"SecureBoot", &sb_global, ATTR_BS | ATTR_RT, 1, &sb);
    set_internal(u"SetupMode", &sb_global, ATTR_BS | ATTR_RT, 1, &sm);
}

void authvar_init(int fixed_keys)                          /* boot time */
{
    fixed = fixed_keys;
    vars_auth_hook = hook;
    vars_after_hook = after;
}

/* The firmware's own write of fixed keys (fw_cfg db/dbx) */
EFI_STATUS authvar_publish(const CHAR16 *name, const EFI_GUID *vendor, const void *data, u32 size)
{
    internal = 1;
    EFI_STATUS st = vars_set((CHAR16 *)name, (EFI_GUID *)vendor, ATTR_BS | ATTR_RT, size, (void *)data);
    internal = 0;
    return st;
}
