/* SquirrelBIOS UEFI core (common/efi): handles, protocols and configuration tables.
 * Original code.
 *
 * A handle is a pool block holding the protocols installed on it. All
 * handles are on one list, oldest first. Notifications and the protocol
 * open-information bookkeeping are not kept: OpenProtocol answers like
 * HandleProtocol, and CloseProtocol succeeds for an installed protocol. */
#include "efi.h"

#define HANDLE_MAGIC 0x656c646e61487153ULL      /* "SqHandle" */
#define HANDLE_PROTOCOLS 16
#define CONFIG_MAX 32

struct prot {
    EFI_GUID guid;
    void *iface;
};

struct handle {
    u64 magic;
    struct handle *next;
    int count;
    struct prot p[HANDLE_PROTOCOLS];
};

static struct handle *handles;
static EFI_CONFIGURATION_TABLE config[CONFIG_MAX];

const EFI_GUID efi_loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static struct handle *valid(EFI_HANDLE h)
{
    for (struct handle *p = handles; p; p = p->next)
        if (p == h && p->magic == HANDLE_MAGIC)
            return p;
    return NULL;
}

static struct prot *find(struct handle *h, const EFI_GUID *g)
{
    for (int i = 0; i < h->count; i++)
        if (guid_eq(&h->p[i].guid, g))
            return &h->p[i];
    return NULL;
}

EFI_STATUS EFIAPI efi_install_protocol_interface(EFI_HANDLE *handle, EFI_GUID *guid, u32 type, void *iface)
{
    if (!handle || !guid || type != 0)
        return EFI_INVALID_PARAMETER;
    struct handle *h;
    if (*handle) {
        h = valid(*handle);
        if (!h)
            return EFI_INVALID_PARAMETER;
        if (find(h, guid))
            return EFI_INVALID_PARAMETER;
        if (h->count == HANDLE_PROTOCOLS)
            return EFI_OUT_OF_RESOURCES;
    } else {
        void *mem;
        if (efi_allocate_pool(EfiBootServicesData, sizeof *h, &mem) != EFI_SUCCESS)
            return EFI_OUT_OF_RESOURCES;
        h = mem;
        memset(h, 0, sizeof *h);
        h->magic = HANDLE_MAGIC;
        struct handle **tail = &handles;
        while (*tail)
            tail = &(*tail)->next;
        *tail = h;
        *handle = h;
    }
    h->p[h->count].guid = *guid;
    h->p[h->count].iface = iface;
    h->count++;
    return EFI_SUCCESS;
}

static void drop_handle(struct handle *h)
{
    for (struct handle **pp = &handles; *pp; pp = &(*pp)->next)
        if (*pp == h) {
            *pp = h->next;
            h->magic = 0;
            efi_free_pool(h);
            return;
        }
}

EFI_STATUS EFIAPI efi_uninstall_protocol_interface(EFI_HANDLE handle, EFI_GUID *guid, void *iface)
{
    struct handle *h = valid(handle);
    if (!h || !guid)
        return EFI_INVALID_PARAMETER;
    struct prot *p = find(h, guid);
    if (!p || p->iface != iface)
        return EFI_NOT_FOUND;
    *p = h->p[--h->count];
    if (h->count == 0)
        drop_handle(h);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI reinstall_protocol_interface(EFI_HANDLE handle, EFI_GUID *guid, void *old, void *new)
{
    struct handle *h = valid(handle);
    if (!h || !guid)
        return EFI_INVALID_PARAMETER;
    struct prot *p = find(h, guid);
    if (!p || p->iface != old)
        return EFI_NOT_FOUND;
    p->iface = new;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, void **iface)
{
    struct handle *h = valid(handle);
    if (!h || !guid || !iface)
        return EFI_INVALID_PARAMETER;
    struct prot *p = find(h, guid);
    if (!p) {
        *iface = NULL;
        return EFI_UNSUPPORTED;
    }
    *iface = p->iface;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI open_protocol(EFI_HANDLE handle, EFI_GUID *guid, void **iface,
                                       EFI_HANDLE agent, EFI_HANDLE controller, u32 attr)
{
    (void)agent;
    (void)controller;
    struct handle *h = valid(handle);
    if (!h || !guid || (!iface && attr != EFI_OPEN_PROTOCOL_TEST_PROTOCOL))
        return EFI_INVALID_PARAMETER;
    struct prot *p = find(h, guid);
    if (iface && attr != EFI_OPEN_PROTOCOL_TEST_PROTOCOL)
        *iface = p ? p->iface : NULL;
    return p ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI close_protocol(EFI_HANDLE handle, EFI_GUID *guid, EFI_HANDLE agent, EFI_HANDLE controller)
{
    (void)agent;
    (void)controller;
    struct handle *h = valid(handle);
    if (!h || !guid)
        return EFI_INVALID_PARAMETER;
    return find(h, guid) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI protocols_per_handle(EFI_HANDLE handle, EFI_GUID ***out, UINTN *count)
{
    struct handle *h = valid(handle);
    if (!h || !out || !count)
        return EFI_INVALID_PARAMETER;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(EFI_GUID *) * (usize)h->count, &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    EFI_GUID **list = mem;
    for (int i = 0; i < h->count; i++)
        list[i] = &h->p[i].guid;
    *out = list;
    *count = (UINTN)h->count;
    return EFI_SUCCESS;
}

static int matches(struct handle *h, EFI_LOCATE_SEARCH_TYPE how, EFI_GUID *guid)
{
    return how == AllHandles || find(h, guid) != NULL;
}

static EFI_STATUS EFIAPI locate_handle(EFI_LOCATE_SEARCH_TYPE how, EFI_GUID *guid, void *key,
                                       UINTN *size, EFI_HANDLE *buf)
{
    (void)key;
    if (!size || how > ByProtocol || (how == ByProtocol && !guid))
        return EFI_INVALID_PARAMETER;
    if (how == ByRegisterNotify)
        return EFI_UNSUPPORTED;
    UINTN n = 0;
    for (struct handle *h = handles; h; h = h->next)
        if (matches(h, how, guid))
            n++;
    if (n == 0)
        return EFI_NOT_FOUND;
    if (*size < n * sizeof(EFI_HANDLE)) {
        *size = n * sizeof(EFI_HANDLE);
        return EFI_BUFFER_TOO_SMALL;
    }
    if (!buf)
        return EFI_INVALID_PARAMETER;
    n = 0;
    for (struct handle *h = handles; h; h = h->next)
        if (matches(h, how, guid))
            buf[n++] = h;
    *size = n * sizeof(EFI_HANDLE);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE how, EFI_GUID *guid, void *key,
                                              UINTN *count, EFI_HANDLE **out)
{
    if (!count || !out)
        return EFI_INVALID_PARAMETER;
    UINTN size = 0;
    EFI_STATUS s = locate_handle(how, guid, key, &size, NULL);
    if (s != EFI_BUFFER_TOO_SMALL)
        return s;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, size, &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    s = locate_handle(how, guid, key, &size, mem);
    if (s != EFI_SUCCESS) {
        efi_free_pool(mem);
        return s;
    }
    *out = mem;
    *count = size / sizeof(EFI_HANDLE);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI locate_protocol(EFI_GUID *guid, void *registration, void **iface)
{
    (void)registration;
    if (!guid || !iface)
        return EFI_INVALID_PARAMETER;
    for (struct handle *h = handles; h; h = h->next) {
        struct prot *p = find(h, guid);
        if (p) {
            *iface = p->iface;
            return EFI_SUCCESS;
        }
    }
    *iface = NULL;
    return EFI_NOT_FOUND;
}

/* Handle, then (GUID, interface) pairs, then NULL. All or nothing. */
static EFI_STATUS EFIAPI install_multiple(EFI_HANDLE *handle, ...)
{
    if (!handle)
        return EFI_INVALID_PARAMETER;
    efi_va_list ap;
    EFI_HANDLE orig = *handle;
    EFI_GUID *done[HANDLE_PROTOCOLS];
    void *ifaces[HANDLE_PROTOCOLS];
    int n = 0;
    EFI_STATUS s = EFI_SUCCESS;
    efi_va_start(ap, handle);
    for (;;) {
        EFI_GUID *g = __builtin_va_arg(ap, EFI_GUID *);
        if (!g)
            break;
        void *iface = __builtin_va_arg(ap, void *);
        if (n == HANDLE_PROTOCOLS) {
            s = EFI_OUT_OF_RESOURCES;
            break;
        }
        s = efi_install_protocol_interface(handle, g, 0, iface);
        if (s != EFI_SUCCESS)
            break;
        done[n] = g;
        ifaces[n++] = iface;
    }
    efi_va_end(ap);
    if (s != EFI_SUCCESS) {
        EFI_HANDLE h = *handle;
        while (n--)
            efi_uninstall_protocol_interface(h, done[n], ifaces[n]);
        *handle = orig;
    }
    return s;
}

static EFI_STATUS EFIAPI uninstall_multiple(EFI_HANDLE handle, ...)
{
    efi_va_list ap;
    EFI_GUID *done[HANDLE_PROTOCOLS];
    void *ifaces[HANDLE_PROTOCOLS];
    int n = 0;
    EFI_STATUS s = EFI_SUCCESS;
    struct handle *h = valid(handle);
    if (!h)
        return EFI_INVALID_PARAMETER;
    efi_va_start(ap, handle);
    for (;;) {
        EFI_GUID *g = __builtin_va_arg(ap, EFI_GUID *);
        if (!g)
            break;
        void *iface = __builtin_va_arg(ap, void *);
        int last = h->count == 1;
        if (n == HANDLE_PROTOCOLS || efi_uninstall_protocol_interface(handle, g, iface) != EFI_SUCCESS) {
            s = EFI_INVALID_PARAMETER;
            break;
        }
        done[n] = g;
        ifaces[n++] = iface;
        if (last)
            break;
    }
    efi_va_end(ap);
    if (s != EFI_SUCCESS && valid(handle))
        while (n--)
            efi_install_protocol_interface(&handle, done[n], 0, ifaces[n]);
    return s;
}

EFI_STATUS EFIAPI efi_install_configuration_table(EFI_GUID *guid, void *table)
{
    if (!guid)
        return EFI_INVALID_PARAMETER;
    UINTN n = efi_st->number_of_table_entries;
    for (UINTN i = 0; i < n; i++) {
        if (!guid_eq(&config[i].vendor_guid, guid))
            continue;
        if (table)
            config[i].vendor_table = table;
        else {
            config[i] = config[n - 1];
            efi_st->number_of_table_entries = n - 1;
        }
        efi_reseal(&efi_st->hdr);
        return EFI_SUCCESS;
    }
    if (!table)
        return EFI_NOT_FOUND;
    if (n == CONFIG_MAX)
        return EFI_OUT_OF_RESOURCES;
    config[n].vendor_guid = *guid;
    config[n].vendor_table = table;
    efi_st->number_of_table_entries = n + 1;
    efi_st->configuration_table = config;
    efi_reseal(&efi_st->hdr);
    return EFI_SUCCESS;
}

/* The length of the device path nodes at dp before its end node */
static UINTN dp_len(const u8 *dp)
{
    UINTN n = efi_dp_size((const EFI_DEVICE_PATH_PROTOCOL *)dp);
    return n >= 4 ? n - 4 : 0;
}

/* Is len a node boundary of dp? */
static int dp_boundary(const u8 *dp, UINTN len)
{
    UINTN at = 0;
    while (at < len) {
        UINTN l = dp[at + 2] | (UINTN)dp[at + 3] << 8;
        if (l < 4)
            return 0;
        at += l;
    }
    return at == len;
}

/* LocateDevicePath: the handle with the protocol whose device path is
 * the longest whole-node prefix of *path; *path moves past that prefix. */
static EFI_STATUS EFIAPI locate_device_path(EFI_GUID *guid, EFI_DEVICE_PATH_PROTOCOL **path,
                                            EFI_HANDLE *device)
{
    if (!guid || !path || !*path)
        return EFI_INVALID_PARAMETER;
    const u8 *want = (const u8 *)*path;
    UINTN total = dp_len(want), best_len = 0;
    struct handle *best = NULL;
    for (struct handle *h = handles; h; h = h->next) {
        struct prot *dp = find(h, &efi_device_path_guid);
        if (!dp || !find(h, guid))
            continue;
        UINTN l = dp_len(dp->iface);
        if (l > total || memcmp(dp->iface, want, l) || !dp_boundary(want, l))
            continue;
        if (!best || l > best_len)
            best = h, best_len = l;
    }
    if (!best)
        return EFI_NOT_FOUND;
    if (!device)
        return EFI_INVALID_PARAMETER;
    *device = best;
    *path = (EFI_DEVICE_PATH_PROTOCOL *)(want + best_len);
    return EFI_SUCCESS;
}

void efi_protocol_init(EFI_BOOT_SERVICES *bs)
{
    bs->locate_device_path = (void *)locate_device_path;
    bs->install_protocol_interface = (void *)efi_install_protocol_interface;
    bs->reinstall_protocol_interface = (void *)reinstall_protocol_interface;
    bs->uninstall_protocol_interface = (void *)efi_uninstall_protocol_interface;
    bs->handle_protocol = (void *)efi_handle_protocol;
    bs->locate_handle = (void *)locate_handle;
    bs->install_configuration_table = (void *)efi_install_configuration_table;
    bs->open_protocol = (void *)open_protocol;
    bs->close_protocol = (void *)close_protocol;
    bs->protocols_per_handle = (void *)protocols_per_handle;
    bs->locate_handle_buffer = (void *)locate_handle_buffer;
    bs->locate_protocol = (void *)locate_protocol;
    bs->install_multiple_protocol_interfaces = (void *)install_multiple;
    bs->uninstall_multiple_protocol_interfaces = (void *)uninstall_multiple;
}
