/* SquirrelBIOS UEFI network stack (roadmap F3/F4). Original code.
 *
 * Shared by x86_64-uefi and the device-tree UEFI core: the net_*.c files
 * here are built into both, against whichever sq.h and efi.h the build
 * puts on the include path, and use only what the two cores have in
 * common (memset/memcpy/printf, the boot services by their efi_* names).
 * Each platform supplies net_client_arch and net_platform_probe(),
 * which finds its NICs: x86_64/platform/net_pci.c (virtio-net on PCI,
 * legacy interface), common/efi/netmmio.c (virtio-net on virtio-mmio).
 *
 *   net_virtio.c  virtio-net over a small transport interface: one
 *                 receive and one transmit queue, polled
 *   net_snp.c     EFI_SIMPLE_NETWORK_PROTOCOL on a handle of its own
 *                 (device path: the platform's, then a MAC node)
 *   net_ip.c      Ethernet, ARP, IPv4, ICMP echo, UDP; a DHCP client and
 *                 a TFTP client
 *   net_tcp.c     a small TCP client (one connection, stop-and-wait out)
 *   net_http.c    an HTTP/1.1 client: Content-Length, chunked, redirects
 *   net_boot.c    PXE-style boot: DHCP, then the boot file over TFTP (or
 *                 HTTP, when DHCP names an http:// URL), loaded and
 *                 started as an EFI image */
#ifndef NET_H
#define NET_H

#include "sq.h"
#include "efi.h"

/* ---------------------------------------------------------------- NICs */

struct net_dev {
    u8 mac[6];
    u16 mtu;                    /* payload bytes after the Ethernet header */
    /* A whole Ethernet frame out; 0 once it is sent */
    int (*send)(struct net_dev *, const void *frame, u32 len);
    /* One received frame into buf (up to max): its length, or 0 */
    int (*recv)(struct net_dev *, void *buf, u32 max);
    /* Stop all DMA (ExitBootServices, Shutdown) */
    void (*stop)(struct net_dev *);
    void *priv;
    EFI_DEVICE_PATH_PROTOCOL *dp;   /* the platform's path to it, or NULL */
};

/* The largest file the TFTP and HTTP clients take (1 GB) */
#define NET_FILE_MAX 0x40000000u

/* Milliseconds since some fixed point (the architecture's counter) */
u64 net_ms(void);
/* Platform: the DHCP client architecture (RFC 4578: 7 x64 UEFI, 11 arm64
 * UEFI, 27 riscv64 UEFI) */
extern const u16 net_client_arch;
/* Platform: find the NICs, net_add() each; the count */
int net_platform_probe(void);
/* net_boot.c: a NIC found: its SNP is installed. dp is the platform's
 * path to it (copied), or NULL */
void net_add(struct net_dev *nd, EFI_DEVICE_PATH_PROTOCOL *dp, const char *what);

/* virtio-net over a transport: a device that has been reset and
 * acknowledged, its features read and written through these */
struct vnet_transport {
    void *ctx;
    int version1;                           /* VIRTIO_F_VERSION_1 offered */
    u32 (*features)(void *ctx, int word);   /* device features, word 0/1 */
    void (*set_features)(void *ctx, int word, u32 v);
    u8 (*get_status)(void *ctx);
    void (*set_status)(void *ctx, u8 s);
    u8 (*config8)(void *ctx, u32 off);      /* device-specific config */
    /* Queue q: its size (0: none) with the rings at desc/avail/used; the
     * legacy transports want them laid out as one block from desc */
    u16 (*queue_max)(void *ctx, u16 q);
    int (*queue_set)(void *ctx, u16 q, u16 size, u64 desc, u64 avail, u64 used);
    void (*notify)(void *ctx, u16 q);
};
/* A working net_dev for the device behind t, or NULL */
struct net_dev *vnet_init(struct vnet_transport *t);

/* -------------------------------------------------------------- the SNP */

#define EFI_SIMPLE_NETWORK_PROTOCOL_GUID \
    { 0xA19832B9, 0xAC25, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

typedef struct { u8 addr[32]; } EFI_MAC_ADDRESS;
typedef union { u32 v4; u8 addr[16]; } EFI_IP_ADDRESS;

typedef struct {
    u32 state;                  /* 0 stopped, 1 started, 2 initialized */
    u32 hw_address_size;
    u32 media_header_size;
    u32 max_packet_size;
    u32 nvram_size;
    u32 nvram_access_size;
    u32 receive_filter_mask;
    u32 receive_filter_setting;
    u32 max_mcast_filter_count;
    u32 mcast_filter_count;
    EFI_MAC_ADDRESS mcast_filter[16];
    EFI_MAC_ADDRESS current_address;
    EFI_MAC_ADDRESS broadcast_address;
    EFI_MAC_ADDRESS permanent_address;
    u8 if_type;
    BOOLEAN mac_address_changeable;
    BOOLEAN multiple_tx_supported;
    BOOLEAN media_present_supported;
    BOOLEAN media_present;
} EFI_SIMPLE_NETWORK_MODE;

typedef struct EFI_SIMPLE_NETWORK_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *start)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (EFIAPI *stop)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (EFIAPI *initialize)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, UINTN, UINTN);
    EFI_STATUS (EFIAPI *reset)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *shutdown)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (EFIAPI *receive_filters)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, u32, u32,
                                         BOOLEAN, UINTN, EFI_MAC_ADDRESS *);
    EFI_STATUS (EFIAPI *station_address)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, BOOLEAN,
                                         EFI_MAC_ADDRESS *);
    EFI_STATUS (EFIAPI *statistics)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, BOOLEAN, UINTN *, void *);
    EFI_STATUS (EFIAPI *mcast_ip_to_mac)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, BOOLEAN,
                                         EFI_IP_ADDRESS *, EFI_MAC_ADDRESS *);
    EFI_STATUS (EFIAPI *nvdata)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, BOOLEAN, UINTN, UINTN, void *);
    EFI_STATUS (EFIAPI *get_status)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, u32 *, void **);
    EFI_STATUS (EFIAPI *transmit)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, UINTN, UINTN, void *,
                                  EFI_MAC_ADDRESS *, EFI_MAC_ADDRESS *, u16 *);
    EFI_STATUS (EFIAPI *receive)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, UINTN *, UINTN *, void *,
                                 EFI_MAC_ADDRESS *, EFI_MAC_ADDRESS *, u16 *);
    EFI_EVENT wait_for_packet;
    EFI_SIMPLE_NETWORK_MODE *mode;
} EFI_SIMPLE_NETWORK_PROTOCOL;

/* The NIC's handle with the SNP (and its device path) on it, or NULL */
EFI_HANDLE net_snp_install(struct net_dev *nd);

/* ------------------------------------------------------------- the IP side */

struct net_cfg {
    u32 ip, mask, gw, server, dns;  /* network byte order as stored: a.b.c.d = bytes */
    char file[128];                 /* DHCP option 67, or the BOOTP file field */
    char server_name[64];           /* option 66 */
};

/* Use nd for everything below */
void net_use(struct net_dev *nd);
struct net_dev *net_current(void);
/* Receive and handle frames for up to ms milliseconds (0: what is there) */
void net_poll(u32 ms);

int net_dhcp(struct net_cfg *cfg);  /* 1 with cfg filled */
/* Read a file by TFTP from cfg->server into a pool buffer: 1 with *buf
 * and *size */
int net_tftp_get(const struct net_cfg *cfg, const char *name, void **buf, u32 *size);

void net_print_ip(u32 ip);
u32 net_ip_parse(const char *s);    /* a.b.c.d, or 0 */
const char *net_vendor_class(void); /* "PXEClient:Arch:xxxxx:UNDI:003016" */
u32 net_my_ip(void);
void net_set_addr(u32 ip, u32 mask, u32 gw);

/* For TCP: send len bytes at buf + 34 (Ethernet and IP headers in front)
 * to dst; 0, or < 0 when there is no way there. Checksum helpers. */
int net_ip_send(u8 *buf, u32 dst, u8 proto, u32 len);
u32 net_csum_add(u32 s, const void *data, u32 n);
u16 net_csum_fold(u32 s);
u32 net_pseudo_sum(u32 src, u32 dst, u8 proto, u32 len);
extern void (*net_tcp_input)(u32 src_ip, const u8 *seg, u32 len);

/* net_tcp.c: one TCP connection at a time. connect: 1 when open, every
 * in-order byte from the peer going to fn; send: 0 once acknowledged;
 * poll: -1 reset, 1 the peer closed, 0 still open */
typedef void (*net_tcp_fn)(const u8 *data, u32 len);
int net_tcp_connect(u32 ip, u16 port, net_tcp_fn fn);
int net_tcp_send(const void *data, u32 len);
int net_tcp_poll(u32 ms);
void net_tcp_close(void);
void net_tcp_abort(void);

/* net_http.c: GET an http://a.b.c.d[:port]/path URL into a pool buffer:
 * 1 with *buf and *size */
int net_http_get(const char *url, void **buf, u32 *size);

/* UDP for the clients above (one socket at a time) */
typedef void (*net_udp_fn)(u32 src_ip, u16 src_port, const u8 *data, u32 len);
void net_udp_bind(u16 port, net_udp_fn fn);
int net_udp_send(u32 dst_ip, u16 src_port, u16 dst_port, const void *data, u32 len);

/* --------------------------------------------------------------- booting */

/* DHCP, then the boot file DHCP names over TFTP, started as an EFI
 * image on the NIC's handle; 1 if an image ran */
int net_boot(struct net_dev *nd, EFI_HANDLE nic);
/* The firmware's hooks (both cores): find the NICs and put an SNP on
 * each; try net_boot on each in turn, 1 if an image ran */
void efi_net_init(void);
int efi_net_boot(void);

#endif
