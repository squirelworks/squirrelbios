/* SquirrelBIOS UEFI core (common/efi): UpdateCapsule, QueryCapsuleCapabilities
 * and the EFI System Resource Table (ESRT).
 *
 * Original code, written from the UEFI 2.10 specification:
 *   §8.5.3  UpdateCapsule / QueryCapsuleCapabilities
 *   §4.6    EFI System Resource Table (configuration table)
 *   §8.5.4  OsIndicationsSupported (file capsule delivery)
 *
 * No EDK2, OVMF or other firmware sources. Header-only capsules update
 * ESRT LastAttempt*. Signed SQCP capsules (body + WIN_CERTIFICATE_UEFI_GUID
 * PKCS#7 detached) are verified via arch_capsule_verify, then applied
 * (LastAttempt* + serial note) or staged for PersistAcrossReset into an
 * NV variable. QEMU smoke does not rewrite the flash image; that remains
 * a follow-up. Failures: message and, for init/staging resource errors,
 * boot halted. */
#include "efi.h"

#define CAPSULE_MAX_BYTES       (1024u * 1024u)
/* NV staging must fit one variable (x86_64 pflash: 32 KB; DT RAM: 256 B). */
#define CAPSULE_STAGE_FALLBACK  (32u * 1024u)
#define CAPSULE_SGL_MAX_BLOCKS  64
#define CAPSULE_FWCFG           "opt/org.squirrelworks/capsule"
#define CAPSULE_STAGE_ATTR      0x7u   /* NV | BS | RT */

static const EFI_GUID esrt_guid = EFI_SYSTEM_RESOURCE_TABLE_GUID;
static const EFI_GUID sysfw_guid = SQUIRREL_SYSTEM_FIRMWARE_GUID;
static const EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;

static EFI_SYSTEM_RESOURCE_TABLE *esrt;
static u32 fw_version_enc;
static CHAR16 stage_name[] = u"SqCapsuleStaging";

static int guid_same(const EFI_GUID *a, const EFI_GUID *b)
{ return !memcmp(a, b, sizeof *a); }

static u32 encode_sq_version(void)
{
    const char *s = SQ_VERSION;
    u32 parts[3] = { 0, 0, 0 };
    int pi = 0; u32 cur = 0;
    for (; *s && pi < 3; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10u + (u32)(*s - '0');
            if (cur > 255u) cur = 255u;
        } else if (*s == '.') { parts[pi++] = cur; cur = 0; }
        else break;
    }
    if (pi < 3) parts[pi] = cur;
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

static EFI_STATUS validate_header(const EFI_CAPSULE_HEADER *h)
{
    if (!h) return EFI_INVALID_PARAMETER;
    if (h->HeaderSize < sizeof(EFI_CAPSULE_HEADER)) return EFI_INVALID_PARAMETER;
    if (h->CapsuleImageSize < h->HeaderSize) return EFI_INVALID_PARAMETER;
    if (h->CapsuleImageSize > CAPSULE_MAX_BYTES) return EFI_BAD_BUFFER_SIZE;
    return EFI_SUCCESS;
}

static u64 sgl_total(EFI_PHYSICAL_ADDRESS list)
{
    u64 total = 0; int hops = 0;
    while (list && hops++ < CAPSULE_SGL_MAX_BLOCKS) {
        EFI_CAPSULE_BLOCK_DESCRIPTOR *d = (EFI_CAPSULE_BLOCK_DESCRIPTOR *)(uptr)list;
        if (d->Length == 0) {
            list = d->Union.ContinuationPointer;
            if (!list) return total;
            continue;
        }
        total += d->Length;
        if (total > CAPSULE_MAX_BYTES) return (u64)-1;
        list = (EFI_PHYSICAL_ADDRESS)(uptr)(d + 1);
    }
    return (u64)-1;
}

static void esrt_note_attempt(u32 version, u32 status)
{
    if (!esrt || !esrt->FwResourceCount) return;
    esrt->Entries[0].LastAttemptVersion = version;
    esrt->Entries[0].LastAttemptStatus = status;
}

static int is_header_only(const EFI_CAPSULE_HEADER *h)
{
    return h->HeaderSize == sizeof(EFI_CAPSULE_HEADER) &&
           h->CapsuleImageSize == sizeof(EFI_CAPSULE_HEADER);
}

static int is_sqcp(const EFI_CAPSULE_HEADER *h)
{
    if (h->HeaderSize < sizeof(EFI_CAPSULE_HEADER) + sizeof(SQUIRREL_FW_CAPSULE_HDR))
        return 0;
    const SQUIRREL_FW_CAPSULE_HDR *sq =
        (const SQUIRREL_FW_CAPSULE_HDR *)((const u8 *)h + sizeof(EFI_CAPSULE_HEADER));
    return sq->Magic == SQUIRREL_FW_CAPSULE_MAGIC;
}

static u32 sqcp_version(const EFI_CAPSULE_HEADER *h)
{
    const SQUIRREL_FW_CAPSULE_HDR *sq =
        (const SQUIRREL_FW_CAPSULE_HDR *)((const u8 *)h + sizeof(EFI_CAPSULE_HEADER));
    return sq->TargetVersion;
}

static void apply_verified(const EFI_CAPSULE_HEADER *h)
{
    u32 ver = sqcp_version(h);
    esrt_note_attempt(ver, LAST_ATTEMPT_STATUS_SUCCESS);
    if (!efi_boot_services_exited())
        printf("capsule: apply ok version=0x%08x (no flash rewrite)\n", ver);
}

static u32 stage_max_bytes(void)
{
    typedef EFI_STATUS (EFIAPI *q_fn)(u32, u64 *, u64 *, u64 *);
    q_fn q = (q_fn)efi_st->runtime_services->query_variable_info;
    u64 max = 0, rem = 0, maxsz = 0;
    if (q && q(CAPSULE_STAGE_ATTR, &max, &rem, &maxsz) == EFI_SUCCESS && maxsz) {
        if (maxsz > CAPSULE_MAX_BYTES) maxsz = CAPSULE_MAX_BYTES;
        return (u32)maxsz;
    }
    return CAPSULE_STAGE_FALLBACK;
}

static EFI_STATUS stage_capsule(const EFI_CAPSULE_HEADER *h)
{
    typedef EFI_STATUS (EFIAPI *set_fn)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    set_fn set = (set_fn)efi_st->runtime_services->set_variable;
    if (!set) return EFI_UNSUPPORTED;
    u32 lim = stage_max_bytes();
    if (h->CapsuleImageSize > lim) {
        if (!efi_boot_services_exited())
            printf("capsule: staging too large for NV (%u > %u)\n",
                   h->CapsuleImageSize, lim);
        return EFI_OUT_OF_RESOURCES;
    }
    EFI_STATUS s = set(stage_name, (EFI_GUID *)&sysfw_guid, CAPSULE_STAGE_ATTR,
                       h->CapsuleImageSize, (void *)h);
    if (s != EFI_SUCCESS) {
        if (!efi_boot_services_exited())
            printf("capsule: staging SetVariable failed 0x%lx\n", (unsigned long)s);
        return EFI_OUT_OF_RESOURCES;
    }
    if (!efi_boot_services_exited())
        printf("capsule: staged version=0x%08x (%u bytes)\n",
               sqcp_version(h), h->CapsuleImageSize);
    return EFI_SUCCESS;
}

static void clear_staging(void)
{
    typedef EFI_STATUS (EFIAPI *set_fn)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    set_fn set = (set_fn)efi_st->runtime_services->set_variable;
    if (set) set(stage_name, (EFI_GUID *)&sysfw_guid, CAPSULE_STAGE_ATTR, 0, NULL);
}

static EFI_STATUS process_signed(EFI_CAPSULE_HEADER *h, int persist, int *want_reset)
{
    EFI_STATUS s = arch_capsule_verify(h);
    if (s == EFI_SECURITY_VIOLATION) {
        esrt_note_attempt(is_sqcp(h) ? sqcp_version(h) : 0,
                          LAST_ATTEMPT_STATUS_ERROR_AUTH_ERROR);
        if (!efi_boot_services_exited())
            printf("capsule: apply refused (auth)\n");
        return s;
    }
    if (s == EFI_UNSUPPORTED) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL);
        if (!efi_boot_services_exited())
            printf("capsule: signed apply unsupported on this arch\n");
        return s;
    }
    if (s != EFI_SUCCESS) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        if (!efi_boot_services_exited())
            printf("capsule: apply refused (format)\n");
        return s;
    }
    if (persist) {
        s = stage_capsule(h);
        if (s != EFI_SUCCESS) {
            esrt_note_attempt(sqcp_version(h), LAST_ATTEMPT_STATUS_ERROR_INSUFFICIENT_RESOURCES);
            return s;
        }
    } else {
        apply_verified(h);
    }
    if (h->Flags & CAPSULE_FLAGS_INITIATE_RESET)
        *want_reset = 1;
    return EFI_SUCCESS;
}

static EFI_STATUS process_one(EFI_CAPSULE_HEADER *h, int persist, int *want_reset)
{
    EFI_STATUS s = validate_header(h);
    if (s != EFI_SUCCESS) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        return s;
    }
    if (!guid_same(&h->CapsuleGuid, &sysfw_guid)) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        if (!efi_boot_services_exited())
            printf("capsule: UpdateCapsule refused unknown CapsuleGuid\n");
        return EFI_INVALID_PARAMETER;
    }
    if ((h->Flags & CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE) &&
        !(h->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET)) {
        if (efi_boot_services_exited()) {
            esrt_note_attempt(fw_version_enc, LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL);
            return EFI_UNSUPPORTED;
        }
        if (efi_install_configuration_table(&h->CapsuleGuid, h) != EFI_SUCCESS) {
            esrt_note_attempt(fw_version_enc, LAST_ATTEMPT_STATUS_ERROR_INSUFFICIENT_RESOURCES);
            return EFI_OUT_OF_RESOURCES;
        }
    }
    if (is_header_only(h)) {
        esrt_note_attempt(fw_version_enc, LAST_ATTEMPT_STATUS_SUCCESS);
        if (h->Flags & CAPSULE_FLAGS_INITIATE_RESET)
            *want_reset = 1;
        return EFI_SUCCESS;
    }
    if (!is_sqcp(h)) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        if (!efi_boot_services_exited())
            printf("capsule: apply refused (not SQCP)\n");
        return EFI_INVALID_PARAMETER;
    }
    return process_signed(h, persist || (h->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET),
                          want_reset);
}

/* 0 = applied, non-zero = refused (caller still clears staging). */
static int apply_pending_buffer(EFI_CAPSULE_HEADER *h, const char *src)
{
    (void)src;
    if (validate_header(h) != EFI_SUCCESS || !guid_same(&h->CapsuleGuid, &sysfw_guid) ||
        !is_sqcp(h)) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        printf("capsule: pending capsule bad format\n");
        return -1;
    }
    EFI_STATUS s = arch_capsule_verify(h);
    if (s != EFI_SUCCESS) {
        esrt_note_attempt(sqcp_version(h),
                          s == EFI_SECURITY_VIOLATION
                              ? LAST_ATTEMPT_STATUS_ERROR_AUTH_ERROR
                              : LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL);
        printf("capsule: pending apply refused\n");
        return -1;
    }
    apply_verified(h);
    return 0;
}

static void apply_staged_variable(void)
{
    typedef EFI_STATUS (EFIAPI *get_fn)(CHAR16 *, EFI_GUID *, u32 *, UINTN *, void *);
    get_fn get = (get_fn)efi_st->runtime_services->get_variable;
    if (!get) return;
    UINTN size = 0; u32 attr = 0;
    EFI_STATUS s = get(stage_name, (EFI_GUID *)&sysfw_guid, &attr, &size, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || !size || size > CAPSULE_MAX_BYTES)
        return;
    EFI_PHYSICAL_ADDRESS base = 0;
    UINTN pages = (size + 4095) / 4096;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, pages, &base) != EFI_SUCCESS) {
        printf("capsule: no memory for staged capsule\n");
        printf("boot halted\n");
        halt();
    }
    void *buf = (void *)(uptr)base;
    size = pages * 4096;
    s = get(stage_name, (EFI_GUID *)&sysfw_guid, &attr, &size, buf);
    if (s != EFI_SUCCESS) { efi_free_pages(base, pages); return; }
    printf("capsule: applying staged capsule (%lu bytes)\n", (unsigned long)size);
    int refused = apply_pending_buffer((EFI_CAPSULE_HEADER *)buf, "staged");
    clear_staging();
    if (refused)
        printf("capsule: cleared refused staged capsule\n");
    efi_free_pages(base, pages);
}

static void apply_fwcfg_capsule(void)
{
    u16 sel; u32 size;
    if (!fwcfg_find(CAPSULE_FWCFG, &sel, &size) || !size) return;
    if (size > CAPSULE_MAX_BYTES) {
        printf("capsule: fw_cfg capsule too big (%u)\n", size);
        printf("boot halted\n");
        halt();
    }
    EFI_PHYSICAL_ADDRESS base = 0;
    UINTN pages = (size + 4095) / 4096;
    if (efi_allocate_pages(AllocateAnyPages, EfiBootServicesData, pages, &base) != EFI_SUCCESS) {
        printf("capsule: no memory for fw_cfg capsule\n");
        printf("boot halted\n");
        halt();
    }
    void *buf = (void *)(uptr)base;
    fwcfg_read(sel, buf, size);
    printf("capsule: applying fw_cfg capsule (%u bytes)\n", size);
    apply_pending_buffer((EFI_CAPSULE_HEADER *)buf, "fw_cfg");
    efi_free_pages(base, pages);
}

EFI_STATUS EFIAPI efi_update_capsule(EFI_CAPSULE_HEADER **headers, UINTN count,
                                     EFI_PHYSICAL_ADDRESS scatter_gather)
{
    if (!count) return EFI_INVALID_PARAMETER;
    if (!headers && !scatter_gather) return EFI_INVALID_PARAMETER;
    /* SGL-only (no header array) is not gathered/verified yet — never
     * claim SUCCESS or skip signature checks (Sarah #193 follow-up). */
    if (!headers) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL);
        if (!efi_boot_services_exited())
            printf("capsule: UpdateCapsule SGL-only unsupported (need header array)\n");
        return EFI_UNSUPPORTED;
    }
    int persist = 0, populate = 0, initiate = 0;
    for (UINTN i = 0; i < count; i++) {
        if (!headers[i]) return EFI_INVALID_PARAMETER;
        if (headers[i]->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET) persist = 1;
        if (headers[i]->Flags & CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE) populate = 1;
        if (headers[i]->Flags & CAPSULE_FLAGS_INITIATE_RESET) initiate = 1;
    }
    /* One NV staging slot (SqCapsuleStaging) — multi PERSIST would
     * last-win; refuse instead of silently dropping capsules. */
    if (persist && count > 1) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INSUFFICIENT_RESOURCES);
        if (!efi_boot_services_exited())
            printf("capsule: UpdateCapsule persist supports one capsule only\n");
        return EFI_UNSUPPORTED;
    }
    if (persist && !scatter_gather) {
        if (!efi_boot_services_exited())
            printf("capsule: UpdateCapsule persist needs ScatterGatherList\n");
        return EFI_INVALID_PARAMETER;
    }
    if (persist) {
        u64 bytes = sgl_total(scatter_gather);
        if (bytes == (u64)-1) {
            esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
            return EFI_INVALID_PARAMETER;
        }
    }
    int want_reset = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_STATUS s = process_one(headers[i], persist, &want_reset);
        if (s != EFI_SUCCESS) return s;
    }
    if (!efi_boot_services_exited())
        printf("capsule: UpdateCapsule n=%lu persist=%d populate=%d reset=%d\n",
               (unsigned long)count, persist, populate, want_reset || initiate);
    if (want_reset || initiate) arch_reset(EfiResetWarm, 0);
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_query_capsule_capabilities(EFI_CAPSULE_HEADER **headers, UINTN count,
                                                 u64 *max_size, EFI_RESET_TYPE *reset_type)
{
    if (!max_size || !reset_type) return EFI_INVALID_PARAMETER;
    if (!count || !headers) return EFI_INVALID_PARAMETER;
    int need_reset = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_STATUS s = validate_header(headers[i]);
        if (s != EFI_SUCCESS) return s;
        if (!guid_same(&headers[i]->CapsuleGuid, &sysfw_guid)) return EFI_UNSUPPORTED;
        if (headers[i]->Flags & (CAPSULE_FLAGS_PERSIST_ACROSS_RESET |
                                 CAPSULE_FLAGS_INITIATE_RESET))
            need_reset = 1;
    }
    *max_size = CAPSULE_MAX_BYTES;
    *reset_type = need_reset ? EfiResetWarm : EfiResetCold;
    return EFI_SUCCESS;
}

static void publish_os_indications(void)
{
    static CHAR16 name[] = u"OsIndicationsSupported";
    u64 bits = EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED |
               EFI_OS_INDICATIONS_CAPSULE_RESULT_VAR_SUPPORTED;
    typedef EFI_STATUS (EFIAPI *set_fn)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    set_fn set = (set_fn)efi_st->runtime_services->set_variable;
    if (!set) return;
    if (set(name, (EFI_GUID *)&global_guid, 0x6, sizeof bits, &bits) != EFI_SUCCESS) {
        if (!efi_boot_services_exited())
            printf("capsule: OsIndicationsSupported not set\n");
    }
}

void efi_capsule_init(void)
{
    fw_version_enc = encode_sq_version();
    EFI_PHYSICAL_ADDRESS base = 0;
    u32 bytes = (u32)(sizeof(EFI_SYSTEM_RESOURCE_TABLE) + sizeof(EFI_SYSTEM_RESOURCE_ENTRY));
    UINTN pages = (bytes + 4095) / 4096;
    if (efi_allocate_pages(AllocateAnyPages, EfiRuntimeServicesData, pages, &base) != EFI_SUCCESS) {
        printf("capsule: no memory for ESRT\n");
        printf("boot halted\n");
        halt();
    }
    esrt = (EFI_SYSTEM_RESOURCE_TABLE *)(uptr)base;
    memset(esrt, 0, pages * 4096);
    esrt->FwResourceCount = 1;
    esrt->FwResourceCountMax = 1;
    esrt->FwResourceVersion = 1;
    esrt->Entries[0].FwClass = sysfw_guid;
    esrt->Entries[0].FwType = ESRT_FW_TYPE_SYSTEMFIRMWARE;
    esrt->Entries[0].FwVersion = fw_version_enc;
    esrt->Entries[0].LowestSupportedFwVersion = fw_version_enc;
    esrt->Entries[0].CapsuleFlags = CAPSULE_FLAGS_PERSIST_ACROSS_RESET;
    esrt->Entries[0].LastAttemptVersion = 0;
    esrt->Entries[0].LastAttemptStatus = LAST_ATTEMPT_STATUS_SUCCESS;
    if (efi_install_configuration_table((EFI_GUID *)&esrt_guid, esrt) != EFI_SUCCESS) {
        printf("capsule: InstallConfigurationTable(ESRT) failed\n");
        printf("boot halted\n");
        halt();
    }
    publish_os_indications();
    printf("capsule: ESRT at %p, 1 resource (system firmware 0x%08x)\n",
           esrt, fw_version_enc);
    arch_capsule_trust_init();
    apply_staged_variable();
    apply_fwcfg_capsule();
}
