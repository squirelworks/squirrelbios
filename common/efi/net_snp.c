/* SquirrelBIOS UEFI: EFI_SIMPLE_NETWORK_PROTOCOL over a net_dev.
 * Original code, written from the UEFI 2.x description of the protocol.
 *
 * One handle per NIC carries the SNP and a device path: the platform's
 * path to the NIC with a MAC address node after it. Transmit is
 * synchronous (the NIC's send waits for the device), so a buffer handed
 * to Transmit is free again at once and GetStatus gives it back on the
 * next call. Receive keeps one frame aside when the caller's buffer is
 * too small, and WaitForPacket is signalled while a frame is waiting. */
#include "net.h"

#define EFI_NOT_STARTED     EFI_ERR(19)
#define EFI_ALREADY_STARTED EFI_ERR(20)

#define RF_UNICAST          0x01
#define RF_MULTICAST        0x02
#define RF_BROADCAST        0x04
#define RF_PROMISCUOUS      0x08
#define RF_PROMISC_MCAST    0x10

#define INT_RECEIVE         0x01
#define INT_TRANSMIT        0x02

#define SNP_FRAME           1536
#define MSG_MAC_ADDR_DP     11

struct snp {
    EFI_SIMPLE_NETWORK_PROTOCOL p;      /* first: the protocol is the struct */
    EFI_SIMPLE_NETWORK_MODE mode;
    struct net_dev *nd;
    void *tx_done;              /* the last buffer sent, until GetStatus */
    u32 pending;                /* bytes of a frame kept back, or 0 */
    u8 frame[SNP_FRAME];
};

static const EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;

static struct snp *S(EFI_SIMPLE_NETWORK_PROTOCOL *p) { return (struct snp *)p; }

static EFI_STATUS need_init(struct snp *s)
{
    if (s->mode.state == 0)
        return EFI_NOT_STARTED;
    if (s->mode.state == 1)
        return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_start(EFI_SIMPLE_NETWORK_PROTOCOL *p)
{
    if (!p)
        return EFI_INVALID_PARAMETER;
    if (S(p)->mode.state != 0)
        return EFI_ALREADY_STARTED;
    S(p)->mode.state = 1;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_stop(EFI_SIMPLE_NETWORK_PROTOCOL *p)
{
    if (!p)
        return EFI_INVALID_PARAMETER;
    if (S(p)->mode.state == 0)
        return EFI_NOT_STARTED;
    S(p)->mode.state = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_initialize(EFI_SIMPLE_NETWORK_PROTOCOL *p, UINTN rx, UINTN tx)
{
    (void)rx;
    (void)tx;
    if (!p)
        return EFI_INVALID_PARAMETER;
    struct snp *s = S(p);
    if (s->mode.state == 0)
        return EFI_NOT_STARTED;
    s->mode.state = 2;
    s->mode.receive_filter_setting = 0;
    s->pending = 0;
    s->tx_done = NULL;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_reset(EFI_SIMPLE_NETWORK_PROTOCOL *p, BOOLEAN extended)
{
    (void)extended;
    if (!p)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    if (st == EFI_SUCCESS)
        S(p)->pending = 0;
    return st;
}

static EFI_STATUS EFIAPI snp_shutdown(EFI_SIMPLE_NETWORK_PROTOCOL *p)
{
    if (!p)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    if (st == EFI_SUCCESS) {
        S(p)->mode.state = 1;
        S(p)->mode.receive_filter_setting = 0;
    }
    return st;
}

static EFI_STATUS EFIAPI snp_receive_filters(EFI_SIMPLE_NETWORK_PROTOCOL *p, u32 enable, u32 disable,
                                             BOOLEAN reset_mcast, UINTN mcast_count,
                                             EFI_MAC_ADDRESS *mcast)
{
    if (!p)
        return EFI_INVALID_PARAMETER;
    struct snp *s = S(p);
    EFI_STATUS st = need_init(s);
    if (st != EFI_SUCCESS)
        return st;
    if ((enable | disable) & ~s->mode.receive_filter_mask)
        return EFI_INVALID_PARAMETER;
    if (!reset_mcast && mcast_count) {
        if (mcast_count > s->mode.max_mcast_filter_count || !mcast)
            return EFI_INVALID_PARAMETER;
        for (UINTN i = 0; i < mcast_count; i++)
            s->mode.mcast_filter[i] = mcast[i];
        s->mode.mcast_filter_count = (u32)mcast_count;
    }
    if (reset_mcast)
        s->mode.mcast_filter_count = 0;
    s->mode.receive_filter_setting = (s->mode.receive_filter_setting | enable) & ~disable;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_station_address(EFI_SIMPLE_NETWORK_PROTOCOL *p, BOOLEAN reset,
                                             EFI_MAC_ADDRESS *mac)
{
    if (!p)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    if (st != EFI_SUCCESS)
        return st;
    if (reset)
        return EFI_SUCCESS;     /* it never changed */
    if (!mac)
        return EFI_INVALID_PARAMETER;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI snp_statistics(EFI_SIMPLE_NETWORK_PROTOCOL *p, BOOLEAN reset,
                                        UINTN *size, void *table)
{
    (void)reset;
    (void)size;
    (void)table;
    if (!p)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    return st != EFI_SUCCESS ? st : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI snp_mcast_ip_to_mac(EFI_SIMPLE_NETWORK_PROTOCOL *p, BOOLEAN ipv6,
                                             EFI_IP_ADDRESS *ip, EFI_MAC_ADDRESS *mac)
{
    if (!p || !ip || !mac)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    if (st != EFI_SUCCESS)
        return st;
    memset(mac, 0, sizeof *mac);
    if (ipv6) {
        if (ip->addr[0] != 0xFF)
            return EFI_INVALID_PARAMETER;
        mac->addr[0] = 0x33;
        mac->addr[1] = 0x33;
        memcpy(mac->addr + 2, ip->addr + 12, 4);
    } else {
        if ((ip->addr[0] & 0xF0) != 0xE0)
            return EFI_INVALID_PARAMETER;
        mac->addr[0] = 0x01;
        mac->addr[1] = 0x00;
        mac->addr[2] = 0x5E;
        mac->addr[3] = ip->addr[1] & 0x7F;
        mac->addr[4] = ip->addr[2];
        mac->addr[5] = ip->addr[3];
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_nvdata(EFI_SIMPLE_NETWORK_PROTOCOL *p, BOOLEAN read, UINTN off,
                                    UINTN size, void *buf)
{
    (void)read;
    (void)off;
    (void)size;
    (void)buf;
    if (!p)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st = need_init(S(p));
    return st != EFI_SUCCESS ? st : EFI_UNSUPPORTED;
}

/* Whether the filters let this frame through */
static int wanted(struct snp *s, const u8 *f)
{
    u32 rf = s->mode.receive_filter_setting;
    if (rf & RF_PROMISCUOUS)
        return 1;
    if (!(f[0] & 1))
        return (rf & RF_UNICAST) && !memcmp(f, s->mode.current_address.addr, 6);
    if (!memcmp(f, s->mode.broadcast_address.addr, 6))
        return (rf & RF_BROADCAST) != 0;
    if (rf & RF_PROMISC_MCAST)
        return 1;
    if (!(rf & RF_MULTICAST))
        return 0;
    for (u32 i = 0; i < s->mode.mcast_filter_count; i++)
        if (!memcmp(f, s->mode.mcast_filter[i].addr, 6))
            return 1;
    return 0;
}

/* A frame the filters pass, kept in s->frame: 1, or 0 for none yet */
static int fetch(struct snp *s)
{
    while (!s->pending) {
        int n = s->nd->recv(s->nd, s->frame, sizeof s->frame);
        if (n <= 0)
            return 0;
        if (n >= 14 && wanted(s, s->frame))
            s->pending = (u32)n;
    }
    return 1;
}

static EFI_STATUS EFIAPI snp_get_status(EFI_SIMPLE_NETWORK_PROTOCOL *p, u32 *int_status, void **tx_buf)
{
    if (!p || (!int_status && !tx_buf))
        return EFI_INVALID_PARAMETER;
    struct snp *s = S(p);
    EFI_STATUS st = need_init(s);
    if (st != EFI_SUCCESS)
        return st;
    if (int_status)
        *int_status = (fetch(s) ? INT_RECEIVE : 0) | (s->tx_done ? INT_TRANSMIT : 0);
    if (tx_buf) {
        *tx_buf = s->tx_done;
        s->tx_done = NULL;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_transmit(EFI_SIMPLE_NETWORK_PROTOCOL *p, UINTN header_size, UINTN size,
                                      void *buf, EFI_MAC_ADDRESS *src, EFI_MAC_ADDRESS *dst,
                                      u16 *protocol)
{
    if (!p || !buf)
        return EFI_INVALID_PARAMETER;
    struct snp *s = S(p);
    EFI_STATUS st = need_init(s);
    if (st != EFI_SUCCESS)
        return st;
    if (header_size) {
        if (header_size != s->mode.media_header_size || !dst || !protocol)
            return EFI_INVALID_PARAMETER;
        u8 *f = buf;
        memcpy(f, dst->addr, 6);
        memcpy(f + 6, src ? src->addr : s->mode.current_address.addr, 6);
        f[12] = (u8)(*protocol >> 8);
        f[13] = (u8)*protocol;
    }
    if (size < s->mode.media_header_size)
        return EFI_BUFFER_TOO_SMALL;
    if (size > s->mode.media_header_size + s->mode.max_packet_size)
        return EFI_INVALID_PARAMETER;
    if (s->nd->send(s->nd, buf, (u32)size))
        return EFI_DEVICE_ERROR;
    s->tx_done = buf;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI snp_receive(EFI_SIMPLE_NETWORK_PROTOCOL *p, UINTN *header_size, UINTN *size,
                                     void *buf, EFI_MAC_ADDRESS *src, EFI_MAC_ADDRESS *dst,
                                     u16 *protocol)
{
    if (!p || !size || !buf)
        return EFI_INVALID_PARAMETER;
    struct snp *s = S(p);
    EFI_STATUS st = need_init(s);
    if (st != EFI_SUCCESS)
        return st;
    if (!fetch(s))
        return EFI_NOT_READY;
    if (*size < s->pending) {
        *size = s->pending;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(buf, s->frame, s->pending);
    *size = s->pending;
    s->pending = 0;
    if (header_size)
        *header_size = 14;
    if (dst) {
        memset(dst, 0, sizeof *dst);
        memcpy(dst->addr, s->frame, 6);
    }
    if (src) {
        memset(src, 0, sizeof *src);
        memcpy(src->addr, s->frame + 6, 6);
    }
    if (protocol)
        *protocol = (u16)(s->frame[12] << 8 | s->frame[13]);
    return EFI_SUCCESS;
}

static void EFIAPI snp_wait(EFI_EVENT ev, void *ctx)
{
    struct snp *s = ctx;
    if (s->mode.state == 2 && fetch(s))
        efi_signal_event(ev);
}

/* The MAC address node (messaging, subtype 11) and an end node */
static EFI_DEVICE_PATH_PROTOCOL *mac_path(const u8 *mac)
{
    u8 *n;
    if (efi_allocate_pool(EfiBootServicesData, 37 + 4, (void **)&n) != EFI_SUCCESS)
        return NULL;
    memset(n, 0, 41);
    n[0] = MESSAGING_DEVICE_PATH;
    n[1] = MSG_MAC_ADDR_DP;
    n[2] = 37;
    memcpy(n + 4, mac, 6);
    n[36] = 1;                  /* Ethernet */
    n[37] = END_DEVICE_PATH_TYPE;
    n[38] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    n[39] = 4;
    return (EFI_DEVICE_PATH_PROTOCOL *)n;
}

EFI_HANDLE net_snp_install(struct net_dev *nd)
{
    struct snp *s;
    if (efi_allocate_pool(EfiBootServicesData, sizeof *s, (void **)&s) != EFI_SUCCESS)
        return NULL;
    memset(s, 0, sizeof *s);
    s->nd = nd;
    EFI_SIMPLE_NETWORK_MODE *m = &s->mode;
    m->state = 0;
    m->hw_address_size = 6;
    m->media_header_size = 14;
    m->max_packet_size = nd->mtu;
    m->receive_filter_mask = RF_UNICAST | RF_MULTICAST | RF_BROADCAST | RF_PROMISCUOUS | RF_PROMISC_MCAST;
    m->max_mcast_filter_count = 16;
    memcpy(m->current_address.addr, nd->mac, 6);
    memcpy(m->permanent_address.addr, nd->mac, 6);
    memset(m->broadcast_address.addr, 0xFF, 6);
    m->if_type = 1;
    m->media_present_supported = 0;
    m->media_present = 1;

    EFI_SIMPLE_NETWORK_PROTOCOL *p = &s->p;
    p->revision = 0x00010000;
    p->start = snp_start;
    p->stop = snp_stop;
    p->initialize = snp_initialize;
    p->reset = snp_reset;
    p->shutdown = snp_shutdown;
    p->receive_filters = snp_receive_filters;
    p->station_address = snp_station_address;
    p->statistics = snp_statistics;
    p->mcast_ip_to_mac = snp_mcast_ip_to_mac;
    p->nvdata = snp_nvdata;
    p->get_status = snp_get_status;
    p->transmit = snp_transmit;
    p->receive = snp_receive;
    p->mode = m;
    if (efi_create_event(EVT_NOTIFY_WAIT, TPL_NOTIFY, snp_wait, s, &p->wait_for_packet) != EFI_SUCCESS)
        p->wait_for_packet = NULL;

    EFI_DEVICE_PATH_PROTOCOL *mp = mac_path(nd->mac), *dp = mp;
    if (nd->dp && mp)
        dp = efi_dp_append(nd->dp, mp);
    EFI_HANDLE h = NULL;
    if (efi_install_protocol_interface(&h, (EFI_GUID *)&snp_guid, 0, p) != EFI_SUCCESS)
        return NULL;
    if (dp)
        efi_install_protocol_interface(&h, (EFI_GUID *)&efi_device_path_guid, 0, dp);
    return h;
}
