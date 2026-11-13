/* SquirrelBIOS UEFI: a serial-over-LAN style console mirror. Original
 * code.
 *
 * With -fw_cfg name=opt/org.squirrelworks/netconsole,string=A.B.C.D:PORT
 * the firmware takes an address by DHCP on the first NIC and from then
 * on copies everything it writes to the serial console (printf, ConOut)
 * to UDP port PORT on A.B.C.D, a line per datagram, from port 6665. Every
 * datagram that comes back to port 6665 from that host is typed into the
 * console, as if it had arrived on the serial line, so ConIn reads it.
 *
 * Output is sent straight to the NIC with the next hop's Ethernet address
 * found once at the start, so a printf from inside the network code never
 * waits on ARP or polls; input is polled only when the console asks for
 * a key, nothing else is using the network stack at that moment and no
 * application has the NIC's Simple Network Protocol initialized. It all
 * stops at ExitBootServices. */
#include "net.h"

#define SOL_PORT    6665
#define NETCON_CFG  "opt/org.squirrelworks/netconsole"
#define LINE_MAX    1024
#define IN_MAX      256
#define ETH_IP_UDP  42

static struct net_dev *snd;
static EFI_HANDLE snic;
static u32 dst_ip, src_ip;
static u16 dst_port;
static u8 hop_mac[6];
static u8 frame[ETH_IP_UDP + LINE_MAX];
static u32 line_len;
static u8 in_ring[IN_MAX];
static u32 in_head, in_tail;
static int busy;

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static void flush(void)
{
    if (!line_len || busy)
        return;
    busy = 1;
    u8 *e = frame, *ip = frame + 14, *u = frame + 34;
    memcpy(e, hop_mac, 6);
    memcpy(e + 6, snd->mac, 6);
    put16(e + 12, 0x0800);
    u32 ulen = 8 + line_len;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (u16)(20 + ulen));
    static u16 ident = 0x5000;
    put16(ip + 4, ident++);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    put16(ip + 10, 0);
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    put16(ip + 10, net_csum_fold(net_csum_add(0, ip, 20)));
    put16(u, SOL_PORT);
    put16(u + 2, dst_port);
    put16(u + 4, (u16)ulen);
    put16(u + 6, 0);
    u16 c = net_csum_fold(net_csum_add(net_pseudo_sum(src_ip, dst_ip, 17, ulen), u, ulen));
    put16(u + 6, c ? c : 0xFFFF);
    u32 len = 34 + ulen;
    if (len < 60) {
        memset(frame + len, 0, 60 - len);
        len = 60;
    }
    snd->send(snd, frame, len);
    line_len = 0;
    busy = 0;
}

static void sol_putc(u8 c)
{
    frame[ETH_IP_UDP + line_len++] = c;
    if (c == '\n' || line_len == LINE_MAX)
        flush();
}

static void sol_input(u32 src, u16 sport, const u8 *d, u32 len)
{
    if (src != dst_ip || sport != dst_port)
        return;
    for (u32 i = 0; i < len; i++) {
        u32 next = (in_head + 1) % IN_MAX;
        if (next == in_tail)
            break;              /* full: the rest is dropped */
        in_ring[in_head] = d[i];
        in_head = next;
    }
}

static int sol_getc(void)
{
    flush();                    /* a prompt without its newline yet */
    if (in_head == in_tail && !busy && !net_busy() && !net_nic_in_use(snic)) {
        busy = 1;
        struct net_dev *was = net_current();
        if (was == snd)
            net_poll(0);
        busy = 0;
    }
    if (in_head == in_tail)
        return -1;
    u8 c = in_ring[in_tail];
    in_tail = (in_tail + 1) % IN_MAX;
    return c;
}

/* "a.b.c.d:port": 1 with the pieces */
static int parse_target(const char *s, u32 *ip, u16 *port)
{
    char a[20];
    u32 n = 0;
    while (s[n] && s[n] != ':' && n < sizeof a - 1) {
        a[n] = s[n];
        n++;
    }
    a[n] = 0;
    if (s[n] != ':' || !(*ip = net_ip_parse(a)))
        return 0;
    u32 p = 0;
    const char *q = s + n + 1;
    if (!*q)
        return 0;
    for (; *q >= '0' && *q <= '9'; q++)
        p = p * 10 + (u32)(*q - '0');
    while (*q == '\n' || *q == '\r' || *q == ' ')
        q++;
    if (*q || !p || p > 65535)
        return 0;
    *port = (u16)p;
    return 1;
}

void net_sol_init(struct net_dev *nd, EFI_HANDLE nic)
{
    u16 sel;
    u32 size;
    char cfgs[64];
    if (!fwcfg_find(NETCON_CFG, &sel, &size))
        return;
    if (size >= sizeof cfgs) {
        printf("net: %s is too long\n", NETCON_CFG);
        return;
    }
    fwcfg_read(sel, cfgs, size);
    cfgs[size] = 0;
    u32 ip;
    u16 port;
    if (!parse_target(cfgs, &ip, &port)) {
        printf("net: console mirror: \"%s\" is not A.B.C.D:PORT\n", cfgs);
        return;
    }
    struct net_cfg cfg;
    net_use(nd);
    if (!net_dhcp(&cfg)) {
        printf("net: console mirror: no answer from DHCP\n");
        return;
    }
    if (!net_resolve(ip, hop_mac)) {
        printf("net: console mirror: no way to ");
        net_print_ip(ip);
        printf("\n");
        return;
    }
    snd = nd;
    snic = nic;
    dst_ip = ip;
    dst_port = port;
    src_ip = cfg.ip;
    net_udp_bind_extra(SOL_PORT, sol_input);
    con_mirror_putc = sol_putc;
    con_mirror_getc = sol_getc;
    printf("net: console mirrored to ");
    net_print_ip(ip);
    printf(":%u from ", port);
    net_print_ip(cfg.ip);
    printf(":%u\n", SOL_PORT);
}

void net_sol_stop(void)
{
    if (!snd)
        return;
    flush();
    con_mirror_putc = NULL;
    con_mirror_getc = NULL;
    net_udp_bind_extra(0, NULL);
    snd = NULL;
}
