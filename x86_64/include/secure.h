/* SquirrelBIOS x86_64-uefi: verify-only Secure Boot (roadmap C4).
 * Original code. */
#ifndef SECURE_H
#define SECURE_H

#include "vars.h"

#define EFI_SECURITY_VIOLATION EFI_ERR(26)

/* sha256.c (FIPS 180-4) */
struct sha256 {
    u32 h[8];
    u64 len;
    u8 buf[64];
    u32 fill;
};
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, usize n);
void sha256_final(struct sha256 *c, u8 out[32]);
void sha256(const void *data, usize n, u8 out[32]);

/* rsa.c: RSASSA-PKCS1-v1_5 with SHA-256 (RFC 8017), moduli up to 4096 bits */
#define RSA_MAX_BYTES 512
struct rsa_key {
    const u8 *n;                /* big-endian modulus, no leading zeros */
    u32 n_len;
    u32 e;
};
int rsa_verify_sha256(const struct rsa_key *k, const u8 hash[32], const u8 *sig, u32 sig_len);

/* der.c: just enough DER for X.509 keys and Authenticode PKCS#7 */
struct der {
    const u8 *p, *end;
};
struct tlv {
    u8 tag;
    const u8 *v;                /* the value */
    u32 len;
    const u8 *raw;              /* tag and length too */
    u32 raw_len;
};
int der_next(struct der *d, struct tlv *t);
int x509_rsa_key(const u8 *cert, u32 len, struct rsa_key *k);

struct authenticode {
    const u8 *digest;           /* the image hash the signer vouches for */
    u32 digest_len;
    const u8 *signed_data;      /* what the signature covers ... */
    u32 signed_len;
    u8 attrs_set;               /* ... with its first byte read as 0x31 */
    const u8 *sig;
    u32 sig_len;
};
int pkcs7_authenticode(const u8 *p, u32 len, struct authenticode *a);

int pkcs7_detached(const u8 *p, u32 len, const u8 content_hash[32], struct authenticode *a);

/* authvar.c: signature lists and authenticated writes to PK, KEK, db, dbx */
extern const EFI_GUID sb_cert_sha256, sb_cert_rsa2048, sb_cert_x509, sb_image_security, sb_global;
typedef int (*sb_sig_fn)(const EFI_GUID *type, const u8 *data, u32 size, void *ctx);
int sb_db_walk(const u8 *data, u32 len, sb_sig_fn fn, void *ctx);   /* -1 malformed */
int sb_entry_key(const EFI_GUID *type, const u8 *data, u32 size, struct rsa_key *k);
void sb_signed_hash(const struct authenticode *a, u8 out[32]);
int sb_setup_mode(void);
void authvar_init(int fixed_keys);
void authvar_set_mode(int secure_boot, int setup);
EFI_STATUS authvar_publish(const CHAR16 *name, const EFI_GUID *vendor, const void *data, u32 size);

/* pe hash and policy: secure.c */
int pe_authenticode_hash(const u8 *img, UINTN size, u8 out[32]);
void secure_init(void);
int secure_boot_on(void);
EFI_STATUS secure_check_image(const u8 *img, UINTN size);

#endif
