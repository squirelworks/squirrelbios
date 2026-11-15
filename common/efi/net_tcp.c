/* SquirrelBIOS UEFI: a small TCP client, one connection at a time, for
 * HTTP boot. Original code, written from RFC 9293 (TCP) and RFC 6691
 * (the MSS option).
 *
 * The client opens, sends with stop-and-wait (each segment retransmitted
 * until it is acknowledged), and takes the peer's data in order only,
 * handing each in-order segment to a callback and acknowledging it at
 * once; anything out of order gets a duplicate ACK and is sent again by
 * the peer. The receive window is the full 64 KB, since the callback
 * keeps up with whatever arrives. Acknowledgements built inside the
 * receive path use a buffer of their own, so they cannot clobber a
 * segment the sender is building. */
#include "net.h"

#define TCP_HDR     20
#define F_FIN       0x01
#define F_SYN       0x02
#define F_RST       0x04
#define F_PSH       0x08
#define F_ACK       0x10
#define OUR_MSS     1460
#define WINDOW      65535

enum { T_CLOSED, T_SYN_SENT, T_OPEN, T_FIN_SENT };

static struct {
    int state;
    int peer_fin, reset;
    u32 rip;
    u16 rport, lport;
    u32 snd_una, snd_nxt, rcv_nxt;
    u32 mss;
    net_tcp_fn fn;
} tc;

static u8 ttx[1600];            /* segments the client sends */
static u8 tack[96];             /* ACKs from the receive path */

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void put32(u8 *p, u32 v) { put16(p, (u16)(v >> 16)); put16(p + 2, (u16)v); }
static u32 get32(const u8 *p) { return (u32)get16(p) << 16 | get16(p + 2); }

/* seq a after b (mod 2^32) */
static int after(u32 a, u32 b) { return (s32)(a - b) > 0; }

static int seg_send(u8 *buf, u8 flags, u32 seq, const void *data, u32 len)
{
    u8 *t = buf + 34;
    u32 hl = (flags & F_SYN) ? TCP_HDR + 4 : TCP_HDR;
    put16(t, tc.lport);
    put16(t + 2, tc.rport);
    put32(t + 4, seq);
    put32(t + 8, (flags & F_ACK) ? tc.rcv_nxt : 0);
    t[12] = (u8)((hl / 4) << 4);
    t[13] = flags;
    put16(t + 14, WINDOW);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & F_SYN) {
        t[20] = 2;              /* MSS */
        t[21] = 4;
        put16(t + 22, OUR_MSS);
    }
    if (len)
        memcpy(t + hl, data, len);
    u32 s = net_pseudo_sum(net_my_ip(), tc.rip, 6, hl + len);
    put16(t + 16, net_csum_fold(net_csum_add(s, t, hl + len)));
    return net_ip_send(buf, tc.rip, 6, hl + len);
}

static void send_ack(void)
{
    seg_send(tack, F_ACK, tc.snd_nxt, NULL, 0);
}

static void tcp_input(u32 src, const u8 *t, u32 len)
{
    if (tc.state == T_CLOSED || src != tc.rip || len < TCP_HDR ||
        get16(t) != tc.rport || get16(t + 2) != tc.lport)
        return;
    u32 hl = (u32)(t[12] >> 4) * 4;
    if (hl < TCP_HDR || hl > len)
        return;
    if (net_csum_fold(net_csum_add(net_pseudo_sum(src, net_my_ip(), 6, len), t, len)))
        return;                 /* damaged */
    u32 seq = get32(t + 4), ack = get32(t + 8);
    u8 fl = t[13];
    const u8 *d = t + hl;
    u32 dl = len - hl;

    if (fl & F_RST) {
        if (tc.state != T_SYN_SENT || ((fl & F_ACK) && ack == tc.snd_nxt)) {
            tc.reset = 1;
            tc.state = T_CLOSED;
        }
        return;
    }
    if (tc.state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != tc.snd_nxt)
            return;
        tc.rcv_nxt = seq + 1;
        tc.snd_una = ack;
        tc.mss = 536;
        for (u32 i = TCP_HDR; i < hl;) {
            u8 k = t[i];
            if (k == 0)
                break;
            if (k == 1) {
                i++;
                continue;
            }
            if (i + 1 >= hl || t[i + 1] < 2)
                break;
            if (k == 2 && t[i + 1] == 4 && i + 4 <= hl)
                tc.mss = get16(t + i + 2);
            i += t[i + 1];
        }
        if (tc.mss > OUR_MSS)
            tc.mss = OUR_MSS;
        if (tc.mss < 64)
            tc.mss = 64;
        tc.state = T_OPEN;
        send_ack();
        return;
    }
    if ((fl & F_ACK) && after(ack, tc.snd_una) && !after(ack, tc.snd_nxt))
        tc.snd_una = ack;
    if (fl & F_SYN) {           /* a repeated SYN-ACK: our ACK was lost */
        send_ack();
        return;
    }
    if (dl) {
        if (seq == tc.rcv_nxt && !tc.peer_fin) {
            if (tc.fn)
                tc.fn(d, dl);
            tc.rcv_nxt += dl;
        }
        send_ack();             /* in order: the ACK; otherwise a duplicate */
    }
    if ((fl & F_FIN) && seq + dl == tc.rcv_nxt && !tc.peer_fin) {
        tc.rcv_nxt++;
        tc.peer_fin = 1;
        send_ack();
    }
}

int net_tcp_connect(u32 ip, u16 port, net_tcp_fn fn)
{
    memset(&tc, 0, sizeof tc);
    tc.rip = ip;
    tc.rport = port;
    tc.lport = (u16)(49152 + ((net_ms() * 7) & 0x3FFF));
    tc.fn = fn;
    u32 iss = (u32)(net_ms() * 2654435761u);
    tc.snd_una = iss;
    tc.snd_nxt = iss + 1;
    tc.state = T_SYN_SENT;
    net_tcp_input = tcp_input;
    for (int tries = 0; tries < 5; tries++) {
        if (seg_send(ttx, F_SYN, iss, NULL, 0) < 0)
            break;
        u64 end = net_ms() + (500u << tries);
        while (net_ms() < end && tc.state == T_SYN_SENT)
            net_poll(10);
        if (tc.state != T_SYN_SENT)
            break;
    }
    if (tc.state != T_OPEN) {
        tc.state = T_CLOSED;
        net_tcp_input = NULL;
        return 0;
    }
    return 1;
}

int net_tcp_send(const void *data, u32 len)
{
    const u8 *p = data;
    while (len) {
        if (tc.state != T_OPEN)
            return -1;
        u32 n = len < tc.mss ? len : tc.mss;
        u32 seq = tc.snd_nxt;
        tc.snd_nxt += n;
        int acked = 0;
        for (int tries = 0; tries < 6 && !acked; tries++) {
            seg_send(ttx, F_ACK | F_PSH, seq, p, n);
            u64 end = net_ms() + (300u << tries);
            while (net_ms() < end && !(acked = !after(tc.snd_nxt, tc.snd_una)) && !tc.reset)
                net_poll(10);
            if (tc.reset)
                return -1;
        }
        if (!acked)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* Handle segments for up to ms: -1 reset, 1 the peer has closed, 0 open */
int net_tcp_poll(u32 ms)
{
    net_poll(ms);
    return tc.reset ? -1 : tc.peer_fin ? 1 : 0;
}

void net_tcp_close(void)
{
    if (tc.state == T_OPEN) {
        u32 seq = tc.snd_nxt;
        tc.snd_nxt++;
        tc.state = T_FIN_SENT;
        for (int tries = 0; tries < 3; tries++) {
            seg_send(ttx, F_FIN | F_ACK, seq, NULL, 0);
            u64 end = net_ms() + 300;
            while (net_ms() < end && after(tc.snd_nxt, tc.snd_una) && !tc.reset)
                net_poll(10);
            if (!after(tc.snd_nxt, tc.snd_una) || tc.reset)
                break;
        }
    }
    tc.state = T_CLOSED;
    net_tcp_input = NULL;
}

/* Close at once: a reset, for when the client gives up on a transfer */
void net_tcp_abort(void)
{
    if (tc.state != T_CLOSED)
        seg_send(ttx, F_RST | F_ACK, tc.snd_nxt, NULL, 0);
    tc.state = T_CLOSED;
    net_tcp_input = NULL;
}
