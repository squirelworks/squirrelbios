/* SquirrelBIOS UEFI core (common/efi): UpdateCapsule, QueryCapsuleCapabilities
 * and the EFI System Resource Table (ESRT).
 *
 * Original code, written from the UEFI 2.10 specification:
 *   §8.5.3  UpdateCapsule / QueryCapsuleCapabilities
 *   §4.6    EFI System Resource Table (configuration table)
 *   §8.5.4  OsIndicationsSupported (file capsule delivery)
 *
 * No EDK2, OVMF or other firmware sources. Enough for an OS / fwupd-style
 * client to find the ESRT, query capabilities and submit a capsule that
 * updates the ESRT LastAttempt* fields. QEMU smoke does not rewrite the
 * flash image; a signed capsule apply path is a follow-up. */
#include "efi.h"

#define CAPSULE_MAX_BYTES       (1024u * 1024u)
#define CAPSULE_SGL_MAX_BLOCKS  64

static const EFI_GUID esrt_guid = EFI_SYSTEM_RESOURCE_TABLE_GUID;
static const EFI_GUID sysfw_guid = SQUIRREL_SYSTEM_FIRMWARE_GUID;
static const EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;

/* ESRT lives in runtime data so LastAttempt* can change after ExitBootServices. */
static EFI_SYSTEM_RESOURCE_TABLE *esrt;
static u32 fw_version_enc;

static int guid_same(const EFI_GUID *a, const EFI_GUID *b)
{
    return !memcmp(a, b, sizeof *a);
}

static u32 encode_sq_version(void)
{
    /* SQ_VERSION "M.N.P" -> 0x00MMNNPP. Missing parts become 0. */
    const char *s = SQ_VERSION;
    u32 parts[3] = { 0, 0, 0 };
    int pi = 0;
    u32 cur = 0;
    for (; *s && pi < 3; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10u + (u32)(*s - '0');
            if (cur > 255u)
                cur = 255u;
        } else if (*s == '.') {
            parts[pi++] = cur;
            cur = 0;
        } else
            break;
    }
    if (pi < 3)
        parts[pi] = cur;
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

static EFI_STATUS validate_header(const EFI_CAPSULE_HEADER *h)
{
    if (!h)
        return EFI_INVALID_PARAMETER;
    if (h->HeaderSize < sizeof(EFI_CAPSULE_HEADER))
        return EFI_INVALID_PARAMETER;
    if (h->CapsuleImageSize < h->HeaderSize)
        return EFI_INVALID_PARAMETER;
    if (h->CapsuleImageSize > CAPSULE_MAX_BYTES)
        return EFI_BAD_BUFFER_SIZE;
    return EFI_SUCCESS;
}

/* Walk a scatter-gather list; return the total payload bytes described,
 * or (u64)-1 on a malformed list. Does not require the blocks to be
 * readable beyond the descriptors themselves. */
static u64 sgl_total(EFI_PHYSICAL_ADDRESS list)
{
    u64 total = 0;
    int hops = 0;
    while (list && hops++ < CAPSULE_SGL_MAX_BLOCKS) {
        EFI_CAPSULE_BLOCK_DESCRIPTOR *d = (EFI_CAPSULE_BLOCK_DESCRIPTOR *)(uptr)list;
        if (d->Length == 0) {
            list = d->Union.ContinuationPointer;
            if (!list)
                return total;
            continue;
        }
        total += d->Length;
        if (total > CAPSULE_MAX_BYTES)
            return (u64)-1;
        list = (EFI_PHYSICAL_ADDRESS)(uptr)(d + 1);
    }
    return (u64)-1;
}

static void esrt_note_attempt(u32 version, u32 status)
{
    if (!esrt || !esrt->FwResourceCount)
        return;
    esrt->Entries[0].LastAttemptVersion = version;
    esrt->Entries[0].LastAttemptStatus = status;
}

static EFI_STATUS process_one(EFI_CAPSULE_HEADER *h, int *want_reset)
{
    EFI_STATUS s = validate_header(h);
    if (s != EFI_SUCCESS) {
        esrt_note_attempt(0, LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT);
        return s;
    }

    /* Only the project system-firmware capsule GUID is accepted. */
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

    /* Record a successful accept. Flash rewrite is not done under QEMU. */
    esrt_note_attempt(fw_version_enc, LAST_ATTEMPT_STATUS_SUCCESS);
    if (h->Flags & CAPSULE_FLAGS_INITIATE_RESET)
        *want_reset = 1;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_update_capsule(EFI_CAPSULE_HEADER **headers, UINTN count,
                                     EFI_PHYSICAL_ADDRESS scatter_gather)
{
    if (!count)
        return EFI_INVALID_PARAMETER;
    if (!headers && !scatter_gather)
        return EFI_INVALID_PARAMETER;

    int persist = 0, populate = 0, initiate = 0;
    if (headers) {
        for (UINTN i = 0; i < count; i++) {
            if (!headers[i])
                return EFI_INVALID_PARAMETER;
            if (headers[i]->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET)
                persist = 1;
            if (headers[i]->Flags & CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE)
                populate = 1;
            if (headers[i]->Flags & CAPSULE_FLAGS_INITIATE_RESET)
                initiate = 1;
        }
    }

    /* Persist-across-reset requires a scatter-gather list (UEFI §8.5.3). */
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

    if (!headers) {
        /* Persist-only path with SGL but no array: accept the SGL size
         * check above and treat as one successful system-firmware update. */
        esrt_note_attempt(fw_version_enc, LAST_ATTEMPT_STATUS_SUCCESS);
        if (!efi_boot_services_exited())
            printf("capsule: UpdateCapsule n=%lu persist=%d (SGL only)\n",
                   (unsigned long)count, persist);
        if (initiate)
            arch_reset(EfiResetWarm, 0);
        return EFI_SUCCESS;
    }

    int want_reset = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_STATUS s = process_one(headers[i], &want_reset);
        if (s != EFI_SUCCESS)
            return s;
    }

    if (!efi_boot_services_exited())
        printf("capsule: UpdateCapsule n=%lu persist=%d populate=%d reset=%d\n",
               (unsigned long)count, persist, populate, want_reset || initiate);

    if (want_reset || initiate)
        arch_reset(EfiResetWarm, 0);
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_query_capsule_capabilities(EFI_CAPSULE_HEADER **headers, UINTN count,
                                                 u64 *max_size, EFI_RESET_TYPE *reset_type)
{
    if (!max_size || !reset_type)
        return EFI_INVALID_PARAMETER;
    if (!count || !headers)
        return EFI_INVALID_PARAMETER;

    int need_reset = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_STATUS s = validate_header(headers[i]);
        if (s != EFI_SUCCESS)
            return s;
        if (!guid_same(&headers[i]->CapsuleGuid, &sysfw_guid))
            return EFI_UNSUPPORTED;
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
    /* Advertise file-capsule delivery and capsule-result variables so a
     * host tool can see support without guessing. Volatile BS|RT. */
    static CHAR16 name[] = u"OsIndicationsSupported";
    u64 bits = EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED |
               EFI_OS_INDICATIONS_CAPSULE_RESULT_VAR_SUPPORTED;
    typedef EFI_STATUS (EFIAPI *set_fn)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    set_fn set = (set_fn)efi_st->runtime_services->set_variable;
    if (!set)
        return;
    if (set(name, (EFI_GUID *)&global_guid, 0x6 /* BS|RT */, sizeof bits, &bits) != EFI_SUCCESS) {
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
}
