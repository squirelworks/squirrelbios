/* SquirrelBIOS UEFI: the NICs the platform found, and a PXE-style boot
 * from them. Original code.
 *
 * Each NIC gets its Simple Network Protocol handle as it is found and is
 * reset when the OS calls ExitBootServices. A network boot asks DHCP for
 * an address and a boot file (sending the RFC 4578 architecture and the
 * PXEClient vendor class, so a PXE server can pick BOOTX64.EFI or
 * BOOTAA64.EFI), reads the file over TFTP from the server DHCP names, or
 * over HTTP when the boot file is an http:// URL (UEFI HTTP boot), and
 * starts it as an EFI image whose device is the NIC's handle. */
#include "net.h"

#define NET_MAX 8

static struct {
    struct net_dev *nd;
    EFI_HANDLE h;
} nics[NET_MAX];
static int nnics;
static EFI_EVENT ebs_event;

static void EFIAPI net_exit_boot_services(EFI_EVENT ev, void *ctx)
{
    (void)ev;
    (void)ctx;
    for (int i = 0; i < nnics; i++)
        nics[i].nd->stop(nics[i].nd);
}

u64 net_ms(void)
{
    static u64 per_ms;
    if (!per_ms) {
        u64 hz = efi_counter_hz();
        per_ms = hz >= 1000 ? hz / 1000 : 1;
    }
    return arch_counter() / per_ms;
}

void net_add(struct net_dev *nd, EFI_DEVICE_PATH_PROTOCOL *dp, const char *what)
{
    if (nnics == NET_MAX)
        return;
    nd->dp = dp;
    const u8 *m = nd->mac;
    printf("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", what, m[0], m[1], m[2], m[3], m[4], m[5]);
    EFI_HANDLE h = net_snp_install(nd);
    if (!h) {
        printf("net: no Simple Network Protocol for it\n");
        nd->stop(nd);
        return;
    }
    nics[nnics].nd = nd;
    nics[nnics].h = h;
    nnics++;
    if (!ebs_event)
        efi_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, net_exit_boot_services, NULL,
                         &ebs_event);
}

void efi_net_init(void)
{
    net_platform_probe();
}

static int http_url(const char *f)
{
    return !memcmp(f, "http://", 7);
}

int net_boot(struct net_dev *nd, EFI_HANDLE nic)
{
    struct net_cfg cfg;
    net_use(nd);
    if (!net_dhcp(&cfg)) {
        printf("net: no answer from DHCP\n");
        return 0;
    }
    printf("net: address ");
    net_print_ip(cfg.ip);
    printf(" from ");
    net_print_ip(cfg.server);
    if (!cfg.file[0]) {
        printf(", no boot file\n");
        return 0;
    }
    printf(", boot file %s\n", cfg.file);
    if (cfg.server && !http_url(cfg.file) && (cfg.server & cfg.mask) != (cfg.ip & cfg.mask)) {
        if (!cfg.gw) {
            printf("net: the boot server is on another network and DHCP gave no router\n");
            return 0;
        }
        printf("net: boot server on another network, through router ");
        net_print_ip(cfg.gw);
        printf("\n");
    }
    void *buf;
    u32 size;
    int http = http_url(cfg.file);
    if (http ? !net_http_get(cfg.file, &buf, &size) : !net_tftp_get(&cfg, cfg.file, &buf, &size))
        return 0;
    printf("net: loaded %u bytes%s\n", size, http ? " over HTTP" : "");

    u64 free_before = efi_free_pages_count();
    EFI_HANDLE h;
    EFI_STATUS s = efi_load_image(0, efi_firmware_handle, NULL, buf, size, &h);
    efi_free_pool(buf);
    if (s != EFI_SUCCESS) {
        printf("efi: LoadImage failed: 0x%lx\n", s);
        return 0;
    }
    EFI_LOADED_IMAGE_PROTOCOL *li;
    efi_handle_protocol(h, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li);
    li->device_handle = nic;
    printf("efi: image of %u bytes loaded at %p\n", size, li->image_base);
    UINTN xsize = 0;
    CHAR16 *xdata = NULL;
    s = efi_start_image(h, &xsize, &xdata);
    printf("efi: %s returned 0x%lx\n", cfg.file, s);
    if (efi_free_pages_count() != free_before)
        printf("efi: %ld pages not given back\n", (s64)(free_before - efi_free_pages_count()));
    return 1;
}

int efi_net_boot(void)
{
    for (int i = 0; i < nnics; i++)
        if (net_boot(nics[i].nd, nics[i].h))
            return 1;
    return 0;
}
