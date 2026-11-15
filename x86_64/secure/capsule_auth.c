/* SquirrelBIOS x86_64-uefi: verify signed system-firmware capsules.
 *
 * Original code. Capsule layout (Phase G): EFI_CAPSULE_HEADER, then
 * SQUIRREL_FW_CAPSULE_HDR ('SQCP'), a body, then WIN_CERTIFICATE_UEFI_GUID
 * with a detached PKCS#7 SignedData over SHA-256 of the bytes before the
 * certificate (UEFI 2.10 §8.5.3; detached PKCS#7 as in §8.2 authenticated
 * variables). Trust: EFI_SIGNATURE_LIST from fw_cfg
 * opt/org.squirrelworks/capsule-db (same ESL shape as Secure Boot db).
 * No EDK2/OVMF sources. */
#include "secure.h"

#define CAPSULE_DB_CFG "opt/org.squirrelworks/capsule-db"
#define TRUST_MAX      8192

static const EFI_GUID cert_pkcs7 = EFI_CERT_TYPE_PKCS7_GUID;

/* Plain statics: this file is in X64_RT_C, so objcopy prefixes .data → .rt.data. */
static u8 trust_esl[TRUST_MAX];
static u32 trust_len;
static int trust_ready;

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }

static int guid_is(const EFI_GUID *a, const EFI_GUID *b)
{
    return !memcmp(a, b, sizeof *a);
}

struct check {
    const u8 *hash;
    const struct authenticode *a;
};

static int verifies(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct check *c = ctx;
    struct rsa_key k;
    return sb_entry_key(type, data, size, &k) &&
           rsa_verify_sha256(&k, c->hash, c->a->sig, c->a->sig_len);
}

static int count_entry(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct rsa_key k;
    if (sb_entry_key(type, data, size, &k))
        ++*(int *)ctx;
    return 0;
}

void x86_capsule_trust_init(void)
{
    u16 sel;
    u32 size;

    trust_ready = 0;
    trust_len = 0;
    if (!fwcfg_find(CAPSULE_DB_CFG, &sel, &size) || !size)
        return;
    if (size > TRUST_MAX) {
        printf("capsule: trust ESL too big (%u)\n", size);
        return;
    }
    fwcfg_read(sel, trust_esl, size);
    int keys = 0;
    if (sb_db_walk(trust_esl, size, count_entry, &keys) < 0 || keys < 1) {
        printf("capsule: trust ESL malformed or empty\n");
        return;
    }
    trust_len = size;
    trust_ready = 1;
    printf("capsule: trust ESL %u bytes (%d key(s))\n", size, keys);
}

u64 x86_capsule_verify(const void *hdr_v)
{
    const EFI_CAPSULE_HEADER *hdr = hdr_v;
    if (!hdr)
        return EFI_INVALID_PARAMETER;
    if (hdr->HeaderSize < sizeof(EFI_CAPSULE_HEADER) + sizeof(SQUIRREL_FW_CAPSULE_HDR))
        return EFI_INVALID_PARAMETER;
    if (hdr->CapsuleImageSize < hdr->HeaderSize)
        return EFI_INVALID_PARAMETER;

    const u8 *raw = (const u8 *)hdr;
    const SQUIRREL_FW_CAPSULE_HDR *sq =
        (const SQUIRREL_FW_CAPSULE_HDR *)(raw + sizeof(EFI_CAPSULE_HEADER));
    if (sq->Magic != SQUIRREL_FW_CAPSULE_MAGIC)
        return EFI_INVALID_PARAMETER;
    if (hdr->HeaderSize != sizeof(EFI_CAPSULE_HEADER) + sizeof(SQUIRREL_FW_CAPSULE_HDR))
        return EFI_INVALID_PARAMETER;

    u64 body_end = (u64)hdr->HeaderSize + sq->BodySize;
    if (body_end > hdr->CapsuleImageSize || body_end < hdr->HeaderSize)
        return EFI_INVALID_PARAMETER;
    u32 win_off = (u32)body_end;
    u32 win_len = hdr->CapsuleImageSize - win_off;
    if (win_len < 24)
        return EFI_INVALID_PARAMETER;

    const u8 *win = raw + win_off;
    u32 dw = rd32(win);
    if (dw < 24 || dw > win_len)
        return EFI_INVALID_PARAMETER;
    if (rd16(win + 4) != 0x0200 || rd16(win + 6) != WIN_CERT_TYPE_EFI_GUID)
        return EFI_INVALID_PARAMETER;
    if (!guid_is((const EFI_GUID *)(win + 8), &cert_pkcs7))
        return EFI_INVALID_PARAMETER;

    if (!trust_ready || !trust_len) {
        if (!efi_boot_services_exited())
            printf("capsule: no trust ESL (need %s)\n", CAPSULE_DB_CFG);
        return EFI_SECURITY_VIOLATION;
    }

    u8 content_hash[32];
    sha256(raw, win_off, content_hash);

    const u8 *cert = win + 24;
    u32 cert_len = dw - 24;
    struct authenticode a;
    if (!pkcs7_detached(cert, cert_len, content_hash, &a)) {
        if (!efi_boot_services_exited())
            printf("capsule: PKCS#7 detached unreadable\n");
        return EFI_SECURITY_VIOLATION;
    }

    u8 sh[32];
    sb_signed_hash(&a, sh);
    struct check chk = { sh, &a };
    if (sb_db_walk(trust_esl, trust_len, verifies, &chk) <= 0) {
        if (!efi_boot_services_exited())
            printf("capsule: signature not trusted\n");
        return EFI_SECURITY_VIOLATION;
    }
    return EFI_SUCCESS;
}
