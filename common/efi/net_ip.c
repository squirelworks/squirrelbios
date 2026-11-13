/* SquirrelBIOS UEFI: Ethernet, ARP, IPv4, ICMP echo, UDP, and the DHCP
 * and TFTP clients the network boot uses. Original code, written from
 * RFC 791/792/768/826 (IP, ICMP, UDP, ARP), RFC 2131/2132 (DHCP),
 * RFC 1350/2347/2348/2349 (TFTP and its options) and the PXE client
 * options of RFC 4578.
 *
 * Polled and single-threaded: net_poll() takes frames from the NIC and
 * hands them on; protocol handlers only record what came in, and the
 * client loops below decide what to send next. Each sender builds its
 * frame in a buffer of its own (the headers in front of the payload), so
 * a reply sent while another frame waits on ARP cannot clobber it. IPv4
 * addresses are kept as four bytes in a u32 (a.b.c.d in memory order). */
#include "net.h"

#define ETH_HDR     14
#define IP_HDR      20
#define UDP_HDR     8
#define ETH_IP      0x0800
#define ETH_ARP     0x0806
#define FRAME_MAX   1536
#define ARP_SLOTS   8

static struct net_dev *nd;
static u32 my_ip, my_mask, my_gw;
static u8 rxf[FRAME_MAX];       /* the frame being handled */
static u8 rtx[FRAME_MAX];       /* ARP and ICMP replies */
static u8 atx[64];              /* ARP requests */
static u8 utx[FRAME_MAX];       /* UDP */
static int in_poll;
static int delivered;           /* a payload went to a handler */

static struct { u32 ip; u8 mac[6]; } arp[ARP_SLOTS];
static int arp_next;

static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static net_udp_fn udp_fn, udp_extra_fn;
static u16 udp_port, udp_extra_port;
void (*net_tcp_input)(u32 src_ip, const u8 *seg, u32 len);

/* -------------------------------------------------------------- helpers */

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 ipget(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static void ipput(u8 *p, u32 v) { memcpy(p, &v, 4); }

u32 net_csum_add(u32 s, const void *data, u32 n)
{
    const u8 *p = data;
    for (; n > 1; p += 2, n -= 2)
        s += (u32)(p[0] << 8 | p[1]);
    if (n)
        s += (u32)p[0] << 8;
    return s;
}

u16 net_csum_fold(u32 s)
{
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

void net_print_ip(u32 ip)
{
    const u8 *b = (const u8 *)&ip;
    printf("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

u32 net_ip_parse(const char *s)
{
    u32 v = 0;
    u8 *b = (u8 *)&v;
    for (int i = 0; i < 4; i++) {
        u32 n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            n = n * 10 + (u32)(*s++ - '0');
            digits++;
        }
        if (!digits || n > 255 || (i < 3 && *s++ != '.'))
            return 0;
        b[i] = (u8)n;
    }
    return *s ? 0 : v;
}

void net_use(struct net_dev *d)
{
    if (d != nd) {
        nd = d;
        my_ip = my_mask = my_gw = 0;
        memset(arp, 0, sizeof arp);
    }
}

struct net_dev *net_current(void) { return nd; }
u32 net_my_ip(void) { return my_ip; }

void net_set_addr(u32 ip, u32 mask, u32 gw)
{
    my_ip = ip;
    my_mask = mask;
    my_gw = gw;
}

/* ------------------------------------------------------------ Ethernet */

static int eth_send(u8 *buf, const u8 *dst, u16 type, u32 len)
{
    memcpy(buf, dst, 6);
    memcpy(buf + 6, nd->mac, 6);
    put16(buf + 12, type);
    len += ETH_HDR;
    if (len < 60) {
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    return nd->send(nd, buf, len);
}

/* ----------------------------------------------------------------- ARP */

static int arp_lookup(u32 ip, u8 *mac)
{
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp[i].ip == ip && ip) {
            memcpy(mac, arp[i].mac, 6);
            return 1;
        }
    return 0;
}

static void arp_learn(u32 ip, const u8 *mac)
{
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp[i].ip == ip) {
            memcpy(arp[i].mac, mac, 6);
            return;
        }
    arp[arp_next].ip = ip;
    memcpy(arp[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_SLOTS;
}

static void arp_frame(u8 *buf, u16 op, const u8 *tmac, u32 tip)
{
    u8 *a = buf + ETH_HDR;
    put16(a, 1);                /* Ethernet */
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, nd->mac, 6);
    ipput(a + 14, my_ip);
    memcpy(a + 18, tmac, 6);
    ipput(a + 24, tip);
}

static void arp_input(const u8 *a, u32 len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
        return;
    u16 op = get16(a + 6);
    u32 sip = ipget(a + 14), tip = ipget(a + 24);
    if (!my_ip || tip != my_ip)
        return;
    if (sip)
        arp_learn(sip, a + 8);
    if (op == 1) {
        arp_frame(rtx, 2, a + 8, sip);
        eth_send(rtx, a + 8, ETH_ARP, 28);
    }
}

/* The next hop's Ethernet address: 1, or 0 when nobody answers */
static int arp_resolve(u32 ip, u8 *mac)
{
    if (ip == 0xFFFFFFFF || (my_mask && ip == (my_ip | ~my_mask))) {
        memcpy(mac, bcast, 6);
        return 1;
    }
    if (arp_lookup(ip, mac))
        return 1;
    if (in_poll)
        return 0;               /* no waiting from inside a handler */
    static const u8 zero[6];
    for (int tries = 0; tries < 4; tries++) {
        arp_frame(atx, 1, zero, ip);
        eth_send(atx, bcast, ETH_ARP, 28);
        u64 end = net_ms() + 250u * (u32)(tries + 1);
        while (net_ms() < end) {
            net_poll(10);
            if (arp_lookup(ip, mac))
                return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ IP */

/* Send len bytes of payload at buf + 34 to dst: 0, -1 when the next hop
 * does not answer ARP, -2 when dst is off this network with no router */
int net_ip_send(u8 *buf, u32 dst, u8 proto, u32 len)
{
    static u16 ident;
    u32 hop = dst;
    if (dst != 0xFFFFFFFF && my_mask && (dst & my_mask) != (my_ip & my_mask)) {
        if (!my_gw)
            return -2;
        hop = my_gw;
    }
    u8 mac[6];
    if (!arp_resolve(hop, mac))
        return -1;
    u8 *ip = buf + ETH_HDR;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (u16)(IP_HDR + len));
    put16(ip + 4, ++ident);
    put16(ip + 6, 0x4000);      /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    ipput(ip + 12, my_ip);
    ipput(ip + 16, dst);
    put16(ip + 10, net_csum_fold(net_csum_add(0, ip, IP_HDR)));
    return nd->send ? (eth_send(buf, mac, ETH_IP, IP_HDR + len) ? -1 : 0) : -1;
}

/* The pseudo-header sum for a TCP or UDP checksum */
u32 net_pseudo_sum(u32 src, u32 dst, u8 proto, u32 len)
{
    u8 ph[12];
    ipput(ph, src);
    ipput(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    put16(ph + 10, (u16)len);
    return net_csum_add(0, ph, 12);
}

static void icmp_input(u32 src, const u8 *d, u32 len)
{
    if (len < 8 || d[0] != 8 || len > FRAME_MAX - ETH_HDR - IP_HDR)
        return;
    u8 *r = rtx + ETH_HDR + IP_HDR;
    memcpy(r, d, len);
    r[0] = 0;                   /* echo reply */
    put16(r + 2, 0);
    put16(r + 2, net_csum_fold(net_csum_add(0, r, len)));
    net_ip_send(rtx, src, 1, len);
}

static void udp_input(u32 src, const u8 *d, u32 len)
{
    if (len < UDP_HDR)
        return;
    u32 ul = get16(d + 4);
    if (ul < UDP_HDR || ul > len)
        return;
    if (udp_fn && get16(d + 2) == udp_port) {
        udp_fn(src, get16(d), d + UDP_HDR, ul - UDP_HDR);
        delivered = 1;
    } else if (udp_extra_fn && get16(d + 2) == udp_extra_port) {
        udp_extra_fn(src, get16(d), d + UDP_HDR, ul - UDP_HDR);
    }
}

static void ip_input(const u8 *p, u32 len)
{
    if (len < IP_HDR || (p[0] >> 4) != 4)
        return;
    u32 hl = (p[0] & 15) * 4u, tot = get16(p + 2);
    if (hl < IP_HDR || tot < hl || tot > len || (get16(p + 6) & 0x3FFF))
        return;                 /* short, or a fragment */
    u32 src = ipget(p + 12), dst = ipget(p + 16);
    if (my_ip && dst != my_ip && dst != 0xFFFFFFFF && dst != (my_ip | ~my_mask))
        return;
    const u8 *d = p + hl;
    u32 dl = tot - hl;
    switch (p[9]) {
    case 1:
        if (my_ip)
            icmp_input(src, d, dl);
        break;
    case 6:
        if (net_tcp_input && my_ip) {
            net_tcp_input(src, d, dl);
            delivered = 1;
        }
        break;
    case 17:
        udp_input(src, d, dl);
        break;
    }
}

/* Handle frames for up to ms milliseconds, back as soon as one of them
 * carried a payload for a handler */
void net_poll(u32 ms)
{
    if (!nd)
        return;
    u64 end = net_ms() + ms;
    in_poll++;
    delivered = 0;
    do {
        int n;
        while (!delivered && (n = nd->recv(nd, rxf, sizeof rxf)) > 0) {
            if (n < ETH_HDR)
                continue;
            u16 type = get16(rxf + 12);
            if (type == ETH_ARP)
                arp_input(rxf + ETH_HDR, (u32)n - ETH_HDR);
            else if (type == ETH_IP)
                ip_input(rxf + ETH_HDR, (u32)n - ETH_HDR);
        }
    } while (!delivered && net_ms() < end);
    in_poll--;
}

/* ----------------------------------------------------------------- UDP */

void net_udp_bind(u16 port, net_udp_fn fn)
{
    udp_port = port;
    udp_fn = fn;
}

void net_udp_bind_extra(u16 port, net_udp_fn fn)
{
    udp_extra_port = port;
    udp_extra_fn = fn;
}

int net_busy(void) { return in_poll > 0; }

int net_resolve(u32 ip, u8 *mac)
{
    u32 hop = ip;
    if (my_mask && (ip & my_mask) != (my_ip & my_mask)) {
        if (!my_gw)
            return 0;
        hop = my_gw;
    }
    return arp_resolve(hop, mac);
}

int net_udp_send(u32 dst, u16 sport, u16 dport, const void *data, u32 len)
{
    if (len > FRAME_MAX - ETH_HDR - IP_HDR - UDP_HDR)
        return -1;
    u8 *u = utx + ETH_HDR + IP_HDR;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (u16)(UDP_HDR + len));
    put16(u + 6, 0);
    memcpy(u + UDP_HDR, data, len);
    u16 c = net_csum_fold(net_csum_add(net_pseudo_sum(my_ip, dst, 17, UDP_HDR + len),
                                       u, UDP_HDR + len));
    put16(u + 6, c ? c : 0xFFFF);
    return net_ip_send(utx, dst, 17, UDP_HDR + len);
}

/* ---------------------------------------------------------------- DHCP */

#define DHCP_CLIENT 68
#define DHCP_SERVER 67

static struct {
    u32 xid;
    int stage;                  /* 0 discovering, 1 requesting, 2 bound, 3 refused */
    u32 offer, server_id;
    struct net_cfg *cfg;
} dh;

static void dhcp_parse(const u8 *d, u32 len, struct net_cfg *c, u8 *type, u32 *sid)
{
    u32 i = 240;
    int overload = 0;
    while (i < len && d[i] != 255) {
        if (d[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + d[i + 1] > len)
            break;
        u8 code = d[i], n = d[i + 1];
        const u8 *v = d + i + 2;
        if (code == 53 && n >= 1)
            *type = v[0];
        else if (code == 54 && n >= 4)
            *sid = ipget(v);
        else if (code == 1 && n >= 4)
            c->mask = ipget(v);
        else if (code == 3 && n >= 4)
            c->gw = ipget(v);
        else if (code == 6 && n >= 4)
            c->dns = ipget(v);
        else if (code == 52 && n >= 1)
            overload = v[0];
        else if (code == 66 && n) {
            u32 m = n < sizeof c->server_name - 1 ? n : sizeof c->server_name - 1;
            memcpy(c->server_name, v, m);
            c->server_name[m] = 0;
        } else if (code == 67 && n) {
            u32 m = n < sizeof c->file - 1 ? n : sizeof c->file - 1;
            memcpy(c->file, v, m);
            c->file[m] = 0;
        }
        i += 2u + n;
    }
    if (!c->file[0] && !(overload & 1) && d[108]) {
        memcpy(c->file, d + 108, 127);
        c->file[127] = 0;
    }
    if (!c->server_name[0] && !(overload & 2) && d[44]) {
        memcpy(c->server_name, d + 44, 63);
        c->server_name[63] = 0;
    }
}

static void dhcp_input(u32 src, u16 sport, const u8 *d, u32 len)
{
    (void)src;
    if (sport != DHCP_SERVER || len < 240 || d[0] != 2 || memcmp(d + 4, &dh.xid, 4) ||
        memcmp(d + 28, nd->mac, 6) || d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
        return;
    struct net_cfg c;
    memset(&c, 0, sizeof c);
    u8 type = 0;
    u32 sid = 0;
    dhcp_parse(d, len, &c, &type, &sid);
    c.ip = ipget(d + 16);
    c.server = ipget(d + 20);
    if (dh.stage == 0 && type == 2) {
        dh.offer = c.ip;
        dh.server_id = sid;
        dh.stage = 1;
    } else if (dh.stage == 1 && type == 5 && (!sid || !dh.server_id || sid == dh.server_id)) {
        if (!c.server) {
            u32 named = c.server_name[0] ? net_ip_parse(c.server_name) : 0;
            c.server = named ? named : sid;
        }
        *dh.cfg = c;
        dh.stage = 2;
    } else if (dh.stage == 1 && type == 6) {
        dh.stage = 3;
    }
}

static u32 dhcp_build(u8 *p, u8 type)
{
    memset(p, 0, 300);
    p[0] = 1;
    p[1] = 1;
    p[2] = 6;
    memcpy(p + 4, &dh.xid, 4);
    put16(p + 10, 0x8000);      /* replies broadcast: no address yet */
    memcpy(p + 28, nd->mac, 6);
    p[236] = 99;
    p[237] = 130;
    p[238] = 83;
    p[239] = 99;
    u32 i = 240;
    p[i++] = 53;
    p[i++] = 1;
    p[i++] = type;
    if (type == 3) {
        p[i++] = 50;
        p[i++] = 4;
        ipput(p + i, dh.offer);
        i += 4;
        if (dh.server_id) {
            p[i++] = 54;
            p[i++] = 4;
            ipput(p + i, dh.server_id);
            i += 4;
        }
    }
    static const u8 params[] = { 1, 3, 6, 43, 60, 66, 67 };
    p[i++] = 55;
    p[i++] = sizeof params;
    memcpy(p + i, params, sizeof params);
    i += sizeof params;
    p[i++] = 93;                /* client system architecture */
    p[i++] = 2;
    put16(p + i, net_client_arch);
    i += 2;
    p[i++] = 94;                /* network interface: UNDI 3.16 */
    p[i++] = 3;
    p[i++] = 1;
    p[i++] = 3;
    p[i++] = 16;
    const char *vc = net_vendor_class();
    u32 vl = (u32)strlen(vc);
    p[i++] = 60;
    p[i++] = (u8)vl;
    memcpy(p + i, vc, vl);
    i += vl;
    p[i++] = 255;
    return i < 300 ? 300 : i;
}

const char *net_vendor_class(void)
{
    static char vc[40];
    static const char head[] = "PXEClient:Arch:";
    memcpy(vc, head, sizeof head - 1);
    char *p = vc + sizeof head - 1;
    u32 a = net_client_arch;
    for (int i = 4; i >= 0; i--) {
        p[i] = (char)('0' + a % 10);
        a /= 10;
    }
    memcpy(p + 5, ":UNDI:003016", 13);
    return vc;
}

int net_dhcp(struct net_cfg *cfg)
{
    u8 pkt[400];
    memset(cfg, 0, sizeof *cfg);
    net_set_addr(0, 0, 0);
    dh.xid = (u32)net_ms() ^ ((u32)nd->mac[2] << 24 | (u32)nd->mac[3] << 16 |
                              (u32)nd->mac[4] << 8 | nd->mac[5]);
    dh.stage = 0;
    dh.cfg = cfg;
    dh.server_id = 0;
    net_udp_bind(DHCP_CLIENT, dhcp_input);
    for (int tries = 0; tries < 4 && dh.stage < 2; tries++) {
        dh.stage = 0;
        u32 n = dhcp_build(pkt, 1);
        net_udp_send(0xFFFFFFFF, DHCP_CLIENT, DHCP_SERVER, pkt, n);
        u64 end = net_ms() + (1000u << tries);
        int asked = 0;
        while (net_ms() < end && dh.stage < 2) {
            net_poll(20);
            if (dh.stage == 1 && !asked) {
                n = dhcp_build(pkt, 3);
                net_udp_send(0xFFFFFFFF, DHCP_CLIENT, DHCP_SERVER, pkt, n);
                asked = 1;
            }
            if (dh.stage == 3)
                break;
        }
    }
    net_udp_bind(0, NULL);
    if (dh.stage != 2)
        return 0;
    if (!cfg->mask) {           /* the class's mask, when none is given */
        u8 a = ((u8 *)&cfg->ip)[0];
        cfg->mask = a < 128 ? 0x000000FF : a < 192 ? 0x0000FFFF : 0x00FFFFFF;
    }
    net_set_addr(cfg->ip, cfg->mask, cfg->gw);
    return 1;
}

/* ---------------------------------------------------------------- TFTP */

#define TFTP_PORT   69
#define TFTP_BLK    1468        /* fills a 1500-byte MTU */

static struct {
    u32 server;
    u16 tid, port;
    u16 block;                  /* the last block taken */
    u32 blksize;
    u8 *buf;
    u32 size, cap, tsize;
    int done;                   /* 1 whole, -1 refused or out of memory */
    int progress;
    int oack;
    char err[64];
} tf;

static int tf_grow(u32 need)
{
    if (need <= tf.cap)
        return 1;
    u32 cap = tf.cap ? tf.cap : 65536;
    while (cap < need)
        cap *= 2;
    void *nb;
    if (efi_allocate_pool(EfiBootServicesData, cap, &nb) != EFI_SUCCESS)
        return 0;
    if (tf.buf) {
        memcpy(nb, tf.buf, tf.size);
        efi_free_pool(tf.buf);
    }
    tf.buf = nb;
    tf.cap = cap;
    return 1;
}

static u32 tf_num(const u8 *s, u32 n)
{
    u32 v = 0;
    for (u32 i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (u32)(s[i] - '0');
    return v;
}

static int tf_ieq(const u8 *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        u8 c = a[i];
        if (c >= 'A' && c <= 'Z')
            c = (u8)(c + 32);
        if (c != (u8)b[i])
            return 0;
    }
    return !b[n];
}

static void tftp_input(u32 src, u16 sport, const u8 *d, u32 len)
{
    if (src != tf.server || len < 4 || tf.done)
        return;
    if (!tf.tid)
        tf.tid = sport;
    else if (sport != tf.tid)
        return;
    u16 op = get16(d);
    if (op == 5) {
        u32 m = len - 4 < sizeof tf.err - 1 ? len - 4 : sizeof tf.err - 1;
        memcpy(tf.err, d + 4, m);
        tf.err[m] = 0;
        tf.done = -1;
    } else if (op == 6 && tf.block == 0 && !tf.oack) {
        u32 i = 2;
        while (i < len) {
            u32 k = i;
            while (i < len && d[i])
                i++;
            u32 kl = i - k, v = ++i;
            while (i < len && d[i])
                i++;
            if (tf_ieq(d + k, "blksize", kl))
                tf.blksize = tf_num(d + v, i - v);
            else if (tf_ieq(d + k, "tsize", kl))
                tf.tsize = tf_num(d + v, i - v);
            i++;
        }
        if (tf.blksize < 8 || tf.blksize > TFTP_BLK)
            tf.blksize = 512;
        tf.oack = 1;
        tf.progress = 1;
    } else if (op == 3) {
        u16 b = get16(d + 2);
        if (b == (u16)(tf.block + 1)) {
            u32 n = len - 4;
            if (n > tf.blksize)
                return;
            if (!tf_grow(tf.size + n)) {
                memcpy(tf.err, "out of memory", 14);
                tf.done = -1;
                return;
            }
            memcpy(tf.buf + tf.size, d + 4, n);
            tf.size += n;
            tf.block = b;
            if (n < tf.blksize)
                tf.done = 1;
        }
        tf.progress = 1;        /* new or repeated, it wants an ACK */
    }
}

static void tftp_ack(void)
{
    u8 a[4];
    put16(a, 4);
    put16(a + 2, tf.block);
    net_udp_send(tf.server, tf.port, tf.tid, a, 4);
}

int net_tftp_get(const struct net_cfg *cfg, const char *name, void **buf, u32 *size)
{
    u8 rrq[300];
    u32 nl = (u32)strlen(name);
    if (nl > 200)
        return 0;
    memset(&tf, 0, sizeof tf);
    tf.server = cfg->server;
    tf.port = (u16)(49152 + (net_ms() & 0x3FFF));
    tf.blksize = 512;
    u32 n = 0;
    put16(rrq, 1);
    n = 2;
    memcpy(rrq + n, name, nl + 1);
    n += nl + 1;
    static const char opts[] = "octet\0blksize\0" "1468\0tsize\0" "0";
    memcpy(rrq + n, opts, sizeof opts);
    n += sizeof opts;
    net_udp_bind(tf.port, tftp_input);
    int ret = 0, quiet = 0;
    if (net_udp_send(tf.server, tf.port, TFTP_PORT, rrq, n)) {
        printf("net: cannot reach the TFTP server ");
        net_print_ip(tf.server);
        printf("\n");
        goto out;
    }
    u64 seen = net_ms();
    while (!tf.done) {
        tf.progress = 0;
        net_poll(50);
        if (tf.done)
            break;
        if (tf.progress) {
            if (tf.oack && tf.tsize && !tf.size && !tf_grow(tf.tsize + 1)) {
                memcpy(tf.err, "out of memory", 14);
                tf.done = -1;
                break;
            }
            tftp_ack();
            seen = net_ms();
            quiet = 0;
        } else if (net_ms() - seen > 1000) {
            if (++quiet > 8) {
                memcpy(tf.err, "timed out", 10);
                tf.done = -1;
                break;
            }
            if (tf.tid)
                tftp_ack();
            else
                net_udp_send(tf.server, tf.port, TFTP_PORT, rrq, n);
            seen = net_ms();
        }
    }
    if (tf.done == 1) {
        tftp_ack();             /* the last block's */
        *buf = tf.buf;
        *size = tf.size;
        if (!tf.buf)            /* an empty file */
            efi_allocate_pool(EfiBootServicesData, 1, buf);
        ret = 1;
    } else {
        printf("net: TFTP of %s failed: %s\n", name, tf.err);
        if (tf.buf)
            efi_free_pool(tf.buf);
    }
out:
    net_udp_bind(0, NULL);
    return ret;
}
