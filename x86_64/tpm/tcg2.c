/* SquirrelBIOS x86_64-uefi: measured boot (roadmap C5).
 * Original code, written from the TCG PC Client Platform Firmware
 * Profile (PCR usage, event types, the crypto-agile event log), the TCG
 * EFI Protocol Specification (EFI_TCG2_PROTOCOL, the final events
 * table), the TPM 2.0 Library commands it sends (TPM2_Startup,
 * TPM2_GetCapability, TPM2_PCR_Extend) and the TCG ACPI Specification
 * (the TPM2 table).
 *
 * At boot: TPM2_Startup(CLEAR), the active PCR banks, then PCR 0 gets
 * the CRTM version and the firmware flash image. Before the first image
 * runs, PCR 7 gets SecureBoot, PK, KEK, db and dbx, PCR 4 the boot
 * action, and PCRs 0-7 a separator. Every image LoadImage accepts goes
 * into PCR 4 (its Authenticode hash), and ExitBootServices into PCR 5.
 * Each extend covers every active bank this firmware can hash (SHA-1,
 * SHA-256, SHA-384, SHA-512) and is logged in the TCG2 (crypto-agile)
 * format; the TPM2 table and GetEventLog give the OS that log. */
#include "tpm.h"
#include "secure.h"
#include "acpi.h"

#define TPM_ST_NO_SESSIONS  0x8001
#define TPM_ST_SESSIONS     0x8002
#define TPM_CC_STARTUP      0x144
#define TPM_CC_PCR_EXTEND   0x182
#define TPM_CC_GET_CAP      0x17A
#define TPM_RC_INITIALIZE   0x100
#define TPM_RS_PW           0x40000009
#define TPM_CAP_PCRS        5
#define TPM_CAP_PROPERTIES  6
#define TPM_PT_MANUFACTURER 0x105

#define EV_NO_ACTION        0x3
#define EV_SEPARATOR        0x4
#define EV_S_CRTM_VERSION   0x8
#define EV_EFI_VARIABLE_DRIVER_CONFIG     0x80000001
#define EV_EFI_BOOT_SERVICES_APPLICATION  0x80000003
#define EV_EFI_ACTION       0x80000007
#define EV_EFI_PLATFORM_FIRMWARE_BLOB     0x80000008

#define EFI_VOLUME_FULL EFI_ERR(11)

#define LOG_SIZE    0x10000
#define FINAL_SIZE  0x4000
#define FLASH_SIZE  0x100000

#ifndef CONFIG_TPM
#define CONFIG_TPM 1
#endif

static const struct bank {
    u16 alg, size;
    u32 bit;                    /* EFI_TCG2_BOOT_HASH_ALG_* */
    const char *name;
} banks[] = {
    { 0x0004, 20, 1, "SHA-1" },
    { 0x000B, 32, 2, "SHA-256" },
    { 0x000C, 48, 4, "SHA-384" },
    { 0x000D, 64, 8, "SHA-512" },
};
#define NBANKS 4

static int present;
static u32 active;              /* bit i: banks[i] is active and extended */
static u32 tpm_banks;           /* every bank the TPM has (EFI bitmap) */
static u32 tpm_active;          /* every active bank (EFI bitmap) */
static u32 manufacturer;
static u8 *log_base, *log_end, *log_next, *log_last;
static int log_truncated, log_given;
static u8 *final_table;
static u32 final_used;
static int ready;

static void put16be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32be(u8 *p, u32 v) { put16be(p, (u16)(v >> 16)); put16be(p + 2, (u16)v); }
static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

/* ---------------------------------------------------------- hashing */

struct mhash {
    struct sha1 s1;
    struct sha256 s256;
    struct sha512 s384, s512;
};

static void mh_init(struct mhash *h)
{
    sha1_init(&h->s1);
    sha256_init(&h->s256);
    sha512_init(&h->s384, 1);
    sha512_init(&h->s512, 0);
}

static void mh_update(void *ctx, const void *p, usize n)
{
    struct mhash *h = ctx;
    if (active & 1)
        sha1_update(&h->s1, p, n);
    if (active & 2)
        sha256_update(&h->s256, p, n);
    if (active & 4)
        sha512_update(&h->s384, p, n);
    if (active & 8)
        sha512_update(&h->s512, p, n);
}

static void mh_final(struct mhash *h, u8 d[NBANKS][64])
{
    if (active & 1)
        sha1_final(&h->s1, d[0]);
    if (active & 2)
        sha256_final(&h->s256, d[1]);
    if (active & 4)
        sha512_final(&h->s384, d[2]);
    if (active & 8)
        sha512_final(&h->s512, d[3]);
}

static void mh_data(const void *p, usize n, u8 d[NBANKS][64])
{
    struct mhash h;
    mh_init(&h);
    mh_update(&h, p, n);
    mh_final(&h, d);
}

/* ---------------------------------------------------------- commands */

static u8 cmd[1024], rsp[4096];

static u32 run(u32 len)
{
    return tis_command(cmd, len, rsp, sizeof rsp);
}

static u32 rc(u32 got)
{
    return got >= 10 ? be32(rsp + 6) : 0xFFFFFFFF;
}

static u32 header(u16 tag, u32 code)
{
    put16be(cmd, tag);
    put32be(cmd + 6, code);
    return 10;
}

static u32 finish(u32 len)
{
    put32be(cmd + 2, len);
    return len;
}

static u32 startup(void)
{
    u32 n = header(TPM_ST_NO_SESSIONS, TPM_CC_STARTUP);
    put16be(cmd + n, 0);                /* TPM_SU_CLEAR */
    return rc(run(finish(n + 2)));
}

static u32 get_cap(u32 cap, u32 prop, u32 count, u32 *got)
{
    u32 n = header(TPM_ST_NO_SESSIONS, TPM_CC_GET_CAP);
    put32be(cmd + n, cap);
    put32be(cmd + n + 4, prop);
    put32be(cmd + n + 8, count);
    *got = run(finish(n + 12));
    return rc(*got);
}

/* TPM_CAP_PCRS: a TPML_PCR_SELECTION; a bank is active when any of its
 * PCRs is selected */
static int read_banks(void)
{
    u32 got;
    if (get_cap(TPM_CAP_PCRS, 0, 1, &got) || got < 19)
        return 0;
    const u8 *p = rsp + 15, *end = rsp + got;
    u32 count = be32(p);
    p += 4;
    for (u32 i = 0; i < count && p + 3 <= end; i++) {
        u16 alg = be16(p);
        u8 sz = p[2];
        if (p + 3 + sz > end)
            return 0;
        int on = 0;
        for (u8 j = 0; j < sz; j++)
            on |= p[3 + j];
        for (int b = 0; b < NBANKS; b++)
            if (banks[b].alg == alg) {
                tpm_banks |= banks[b].bit;
                if (on) {
                    tpm_active |= banks[b].bit;
                    active |= 1u << b;
                }
            }
        if (alg == 0x0012) {            /* SM3: we can't hash it */
            tpm_banks |= 0x10;
            if (on) {
                tpm_active |= 0x10;
                printf("tpm: the SM3 bank is active, but not extended\n");
            }
        }
        p += 3 + sz;
    }
    return 1;
}

static void read_manufacturer(void)
{
    u32 got;
    if (!get_cap(TPM_CAP_PROPERTIES, TPM_PT_MANUFACTURER, 1, &got) && got >= 27)
        manufacturer = be32(rsp + 23);
}

static int extend(u32 pcr, u8 d[NBANKS][64])
{
    u32 n = header(TPM_ST_SESSIONS, TPM_CC_PCR_EXTEND);
    put32be(cmd + n, pcr);
    put32be(cmd + n + 4, 9);            /* the password session, empty */
    put32be(cmd + n + 8, TPM_RS_PW);
    put16be(cmd + n + 12, 0);
    cmd[n + 14] = 0;
    put16be(cmd + n + 15, 0);
    n += 17;
    u32 cnt = n;
    n += 4;
    u32 k = 0;
    for (int b = 0; b < NBANKS; b++)
        if (active & 1u << b) {
            put16be(cmd + n, banks[b].alg);
            memcpy(cmd + n + 2, d[b], banks[b].size);
            n += 2 + banks[b].size;
            k++;
        }
    put32be(cmd + cnt, k);
    return rc(run(finish(n))) == 0;
}

/* ---------------------------------------------------------- the log */

/* A TCG_PCR_EVENT2 at p (or its size, with p NULL) */
static u32 event2(u8 *p, u32 pcr, u32 type, u8 d[NBANKS][64], const void *ev, u32 len)
{
    u32 size = 12;
    for (int b = 0; b < NBANKS; b++)
        if (active & 1u << b)
            size += 2 + banks[b].size;
    size += 4 + len;
    if (!p)
        return size;
    put32(p, pcr);
    put32(p + 4, type);
    u32 k = 0, at = 12;
    for (int b = 0; b < NBANKS; b++)
        if (active & 1u << b) {
            u16 alg = banks[b].alg;
            memcpy(p + at, &alg, 2);
            memcpy(p + at + 2, d[b], banks[b].size);
            at += 2 + banks[b].size;
            k++;
        }
    put32(p + 8, k);
    put32(p + at, len);
    memcpy(p + at + 4, ev, len);
    return size;
}

static void log_event(u32 pcr, u32 type, u8 d[NBANKS][64], const void *ev, u32 len)
{
    u32 size = event2(NULL, pcr, type, d, ev, len);
    if (log_truncated || (u64)(log_end - log_next) < size) {
        log_truncated = 1;
        return;
    }
    event2(log_next, pcr, type, d, ev, len);
    log_last = log_next;
    log_next += size;
    /* After GetEventLog the OS has its copy: later events go in the
     * final events table too */
    if (log_given && final_table && final_used + size <= FINAL_SIZE) {
        event2(final_table + final_used, pcr, type, d, ev, len);
        final_used += size;
        u64 n;
        memcpy(&n, final_table + 8, 8);
        put64(final_table + 8, n + 1);
    }
}

/* The first entry: a TCG_PCClientPCREvent (SHA-1 format) of type
 * EV_NO_ACTION carrying the Spec ID Event03 structure */
static void log_header(void)
{
    u8 *p = log_base;
    memset(p, 0, 32);
    put32(p + 4, EV_NO_ACTION);
    u8 *e = p + 32;
    memcpy(e, "Spec ID Event03", 16);
    put32(e + 16, 0);                   /* platform class: client */
    e[20] = 0;                          /* spec version 2.0, errata 2 */
    e[21] = 2;
    e[22] = 2;
    e[23] = 2;                          /* UINTN is 8 bytes */
    u32 k = 0, at = 28;
    for (int b = 0; b < NBANKS; b++)
        if (active & 1u << b) {
            u16 alg = banks[b].alg, sz = banks[b].size;
            memcpy(e + at, &alg, 2);
            memcpy(e + at + 2, &sz, 2);
            at += 4;
            k++;
        }
    put32(e + 24, k);
    e[at++] = 0;                        /* no vendor info */
    put32(p + 28, at);
    log_last = p;
    log_next = e + at;
}

/* Extend pcr with the digests and log the event (unless log is 0) */
static int measure(u32 pcr, u32 type, u8 d[NBANKS][64], const void *ev, u32 len, int log)
{
    if (!present)
        return 0;
    if (!extend(pcr, d)) {
        printf("tpm: PCR %u extend failed\n", pcr);
        return 0;
    }
    if (log)
        log_event(pcr, type, d, ev, len);
    return 1;
}

/* The event data is what is hashed */
static void measure_event(u32 pcr, u32 type, const void *ev, u32 len)
{
    u8 d[NBANKS][64];
    mh_data(ev, len, d);
    measure(pcr, type, d, ev, len, 1);
}

/* ---------------------------------------------------------- measurements */

static void measure_firmware(void)
{
    static const CHAR16 version[] = u"SquirrelBIOS " SQ_VERSION;
    measure_event(0, EV_S_CRTM_VERSION, version, sizeof version);
    u8 blob[16], d[NBANKS][64];
    u64 base = 0x100000000ull - FLASH_SIZE;
    put64(blob, base);
    put64(blob + 8, FLASH_SIZE);
    mh_data((const void *)(uptr)base, FLASH_SIZE, d);
    measure(0, EV_EFI_PLATFORM_FIRMWARE_BLOB, d, blob, 16, 1);
}

static const EFI_GUID global_var = { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };
static const EFI_GUID image_security = { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } };

/* EV_EFI_VARIABLE_DRIVER_CONFIG: a UEFI_VARIABLE_DATA (vendor, name
 * length, data length, name without its NUL, data), hashed whole */
static void measure_variable(const CHAR16 *name, const EFI_GUID *vendor)
{
    u32 chars = 0;
    while (name[chars])
        chars++;
    UINTN size = 0;
    typedef EFI_STATUS (EFIAPI *get_fn)(CHAR16 *, EFI_GUID *, u32 *, UINTN *, void *);
    get_fn get = (get_fn)efi_st->runtime_services->get_variable;
    get((CHAR16 *)name, (EFI_GUID *)vendor, NULL, &size, NULL);
    u32 len = 32 + chars * 2 + (u32)size;
    u8 *ev;
    if (efi_allocate_pool(EfiBootServicesData, len, (void **)&ev) != EFI_SUCCESS)
        return;
    memcpy(ev, vendor, 16);
    put64(ev + 16, chars);
    memcpy(ev + 32, name, chars * 2);
    if (size && get((CHAR16 *)name, (EFI_GUID *)vendor, NULL, &size, ev + 32 + chars * 2))
        size = 0;
    put64(ev + 24, size);
    len = 32 + chars * 2 + (u32)size;
    measure_event(7, EV_EFI_VARIABLE_DRIVER_CONFIG, ev, len);
    efi_free_pool(ev);
}

/* Once, before the first image: the Secure Boot configuration, the
 * boot action and the separators that close PCRs 0-7 */
static void ready_to_boot(void)
{
    if (ready || !present)
        return;
    ready = 1;
    measure_variable(u"SecureBoot", &global_var);
    measure_variable(u"PK", &global_var);
    measure_variable(u"KEK", &global_var);
    measure_variable(u"db", &image_security);
    measure_variable(u"dbx", &image_security);
    static const char action[] = "Calling EFI Application from Boot Option";
    measure_event(4, EV_EFI_ACTION, action, sizeof action - 1);
    u32 zero = 0;
    for (u32 pcr = 0; pcr < 8; pcr++)
        measure_event(pcr, EV_SEPARATOR, &zero, 4);
}

static int image_digests(const u8 *img, UINTN size, u8 d[NBANKS][64])
{
    struct mhash h;
    mh_init(&h);
    if (!pe_authenticode_walk(img, size, mh_update, &h))
        return 0;
    mh_final(&h, d);
    return 1;
}

void tpm_measure_image(const u8 *img, UINTN size)
{
    if (!present)
        return;
    ready_to_boot();
    u8 d[NBANKS][64];
    if (!image_digests(img, size, d))
        return;
    /* UEFI_IMAGE_LOAD_EVENT: where it is, its size, its link-time
     * base, no device path */
    u8 ev[32];
    u32 pe = 0;
    memcpy(&pe, img + 0x3C, 4);
    u64 link = 0;
    memcpy(&link, img + pe + 24 + 24, 8);
    put64(ev, (u64)(uptr)img);
    put64(ev + 8, size);
    put64(ev + 16, link);
    put64(ev + 24, 0);
    measure(4, EV_EFI_BOOT_SERVICES_APPLICATION, d, ev, 32, 1);
}

static void EFIAPI on_exit_boot_services(EFI_EVENT ev, void *ctx)
{
    (void)ev, (void)ctx;
    static const char action[] = "Exit Boot Services Invocation";
    measure_event(5, EV_EFI_ACTION, action, sizeof action - 1);
}

/* ---------------------------------------------------------- EFI_TCG2_PROTOCOL */

static const EFI_GUID tcg2_guid = { 0x607f766c, 0x7455, 0x42be, { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } };
static const EFI_GUID final_guid = { 0x1e2ed096, 0x30e2, 0x4254, { 0xbd, 0x89, 0x86, 0x3b, 0xbe, 0xf8, 0x23, 0x25 } };

#define CAP_SIZE 30

/* EFI_TCG2_BOOT_SERVICE_CAPABILITY, packed */
static EFI_STATUS EFIAPI get_capability(void *this, u8 *cap)
{
    if (!this || !cap)
        return EFI_INVALID_PARAMETER;
    if (cap[0] < CAP_SIZE) {
        cap[0] = CAP_SIZE;
        return EFI_BUFFER_TOO_SMALL;
    }
    memset(cap, 0, CAP_SIZE);
    cap[0] = CAP_SIZE;
    cap[1] = 1, cap[2] = 1;             /* structure version 1.1 */
    cap[3] = 1, cap[4] = 1;             /* protocol version 1.1 */
    put32(cap + 5, 0x0F);               /* SHA-1, SHA-256, SHA-384, SHA-512 */
    put32(cap + 9, 2);                  /* the TCG2 (crypto-agile) log */
    cap[13] = (u8)present;
    u16 max = sizeof cmd;
    memcpy(cap + 14, &max, 2);
    max = sizeof rsp;
    memcpy(cap + 16, &max, 2);
    put32(cap + 18, manufacturer);
    u32 nb = 0;
    for (u32 b = tpm_banks; b; b >>= 1)
        nb += b & 1;
    put32(cap + 22, nb);
    put32(cap + 26, tpm_active);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_event_log(void *this, u32 format, u64 *where, u64 *last, BOOLEAN *truncated)
{
    if (!this || !where || !last || !truncated)
        return EFI_INVALID_PARAMETER;
    if (format != 2)
        return EFI_INVALID_PARAMETER;   /* only the crypto-agile log */
    if (!present) {
        *where = *last = 0;
        *truncated = 0;
        return EFI_SUCCESS;
    }
    *where = (u64)(uptr)log_base;
    *last = (u64)(uptr)log_last;
    *truncated = (BOOLEAN)log_truncated;
    log_given = 1;
    return EFI_SUCCESS;
}

#define PE_COFF_IMAGE   0x10
#define EXTEND_ONLY     0x01

/* EFI_TCG2_EVENT: Size, then a packed header (HeaderSize 14,
 * HeaderVersion 1, PCRIndex, EventType), then the event data */
static EFI_STATUS EFIAPI hash_log_extend(void *this, u64 flags, u64 data, u64 len, const u8 *ev)
{
    if (!this || !ev || (!data && len))
        return EFI_INVALID_PARAMETER;
    u32 size, hsize, pcr, type;
    u16 hver;
    memcpy(&size, ev, 4);
    memcpy(&hsize, ev + 4, 4);
    memcpy(&hver, ev + 8, 2);
    memcpy(&pcr, ev + 10, 4);
    memcpy(&type, ev + 14, 4);
    if (hsize < 14 || size < 4 + hsize || pcr > 23 || hver != 1)
        return EFI_INVALID_PARAMETER;
    if (!present)
        return EFI_DEVICE_ERROR;
    u8 d[NBANKS][64];
    if (flags & PE_COFF_IMAGE) {
        if (!image_digests((const u8 *)(uptr)data, len, d))
            return EFI_UNSUPPORTED;
    } else {
        mh_data((const void *)(uptr)data, len, d);
    }
    if (!measure(pcr, type, d, ev + 4 + hsize, size - 4 - hsize, !(flags & EXTEND_ONLY)))
        return EFI_DEVICE_ERROR;
    if (log_truncated && !(flags & EXTEND_ONLY))
        return EFI_VOLUME_FULL;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI submit_command(void *this, u32 in_size, const u8 *in, u32 out_size, u8 *out)
{
    if (!this || !in || !out || in_size < 10)
        return EFI_INVALID_PARAMETER;
    if (!present)
        return EFI_DEVICE_ERROR;
    if (in_size > sizeof cmd)
        return EFI_BUFFER_TOO_SMALL;
    memcpy(cmd, in, in_size);
    u32 got = run(in_size);
    if (!got)
        return EFI_DEVICE_ERROR;
    if (got > out_size)
        return EFI_BUFFER_TOO_SMALL;
    memcpy(out, rsp, got);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_active_banks(void *this, u32 *out)
{
    if (!this || !out)
        return EFI_INVALID_PARAMETER;
    *out = tpm_active;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_active_banks(void *this, u32 banks_)
{
    (void)this, (void)banks_;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI get_result_set_banks(void *this, u32 *op, u32 *resp)
{
    (void)this, (void)op, (void)resp;
    return EFI_UNSUPPORTED;
}

static struct {
    void *get_capability, *get_event_log, *hash_log_extend_event, *submit_command,
         *get_active_pcr_banks, *set_active_pcr_banks, *get_result_of_set_active_pcr_banks;
} tcg2 = {
    (void *)get_capability, (void *)get_event_log, (void *)hash_log_extend, (void *)submit_command,
    (void *)get_active_banks, (void *)set_active_banks, (void *)get_result_set_banks,
};

/* ---------------------------------------------------------- init, ACPI */

int tpm_present(void)
{
    return present;
}

void tpm_init(void)
{
    u32 id;
    if (!CONFIG_TPM || !tis_probe(&id)) {
        printf("tpm: none\n");
        return;
    }
    u32 r = startup();
    if (r && r != TPM_RC_INITIALIZE) {
        printf("tpm: TIS device %04x:%04x, but TPM2_Startup failed (0x%x)\n", id & 0xFFFF, id >> 16, r);
        return;
    }
    if (!read_banks() || !active) {
        printf("tpm: TIS device %04x:%04x, but no PCR bank it can extend\n", id & 0xFFFF, id >> 16);
        return;
    }
    read_manufacturer();
    EFI_PHYSICAL_ADDRESS lb = 0xFFFFFFFF, fb = 0xFFFFFFFF;
    if (efi_allocate_pages(AllocateMaxAddress, EfiACPIMemoryNVS, LOG_SIZE / 4096, &lb) ||
        efi_allocate_pages(AllocateMaxAddress, EfiACPIMemoryNVS, FINAL_SIZE / 4096, &fb)) {
        printf("tpm: no memory for the event log\n");
        return;
    }
    log_base = (u8 *)(uptr)lb;
    log_end = log_base + LOG_SIZE;
    memset(log_base, 0, LOG_SIZE);
    final_table = (u8 *)(uptr)fb;
    memset(final_table, 0, FINAL_SIZE);
    put64(final_table, 1);              /* version; no events yet */
    final_used = 16;
    present = 1;
    log_header();

    printf("tpm: TPM 2.0 at 0x%x (TIS, %04x:%04x), PCR banks", TPM_TIS_BASE, id & 0xFFFF, id >> 16);
    for (int b = 0; b < NBANKS; b++)
        if (active & 1u << b)
            printf(" %s", banks[b].name);
    printf("\n");
    measure_firmware();

    EFI_HANDLE h = NULL;
    EFI_EVENT ev;
    if (efi_install_protocol_interface(&h, (EFI_GUID *)&tcg2_guid, 0, &tcg2) ||
        efi_install_configuration_table((EFI_GUID *)&final_guid, final_table) ||
        efi_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK, on_exit_boot_services, NULL, &ev))
        printf("tpm: EFI_TCG2_PROTOCOL not installed\n");
}

/* TPM2 (revision 4): a client platform, the FIFO interface (start
 * method 6, no control area) and the event log */
int tpm_acpi_tables(u8 **out, void *(*take)(u32 len))
{
    if (!present)
        return 0;
    u8 *t = take(76);
    if (!t)
        return 0;
    memset(t + 36, 0, 40);
    put32(t + 48, 6);
    put32(t + 64, LOG_SIZE);
    put64(t + 68, (u64)(uptr)log_base);
    acpi_header(t, "TPM2", 76, 4);
    acpi_seal(t);
    out[0] = t;
    return 1;
}
