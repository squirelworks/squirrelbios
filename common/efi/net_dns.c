/* SquirrelBIOS UEFI: a DNS A-record client for HTTP boot. Original code,
 * written from RFC 1035 (Domain Names — Implementation and Specification).
 *
 * One question at a time over UDP/53 to the DNS server DHCP named (option
 * 6). A positive answer with an A record yields the IPv4 address; anything
 * else (timeout, NXDOMAIN, a truncated or malformed reply) is a miss. */
#include "net.h"

#define DNS_PORT    53
#define DNS_HDR     12
#define DNS_TRIES   4
#define QTYPE_A     1
#define QCLASS_IN   1
#define RCODE_MASK  0x000F

static struct {
    u16 id;
    u16 sport;
    int done;                   /* 1 answer, -1 refused or bad */
    u32 ip;
} dq;

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

/* Encode a dotted name as DNS labels into out; the trailing 0 is included.
 * 0 on overflow or an empty/illegal label. */
static u32 dns_name_encode(u8 *out, u32 max, const char *name)
{
    u32 i = 0;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.')
            dot++;
        u32 lab = (u32)(dot - name);
        if (!lab || lab > 63 || i + 1 + lab >= max)
            return 0;
        out[i++] = (u8)lab;
        memcpy(out + i, name, lab);
        i += lab;
        name = *dot ? dot + 1 : dot;
    }
    if (i + 1 > max)
        return 0;
    out[i++] = 0;
    return i;
}

/* Skip a compressed or plain name; the byte after it, or 0 on error. */
static u32 dns_name_skip(const u8 *msg, u32 len, u32 off)
{
    u32 jumps = 0;
    while (off < len) {
        u8 n = msg[off];
        if (!n)
            return off + 1;
        if ((n & 0xC0) == 0xC0) {
            if (off + 1 >= len || ++jumps > 16)
                return 0;
            return off + 2;
        }
        if ((n & 0xC0) || off + 1 + n > len)
            return 0;
        off += 1u + n;
    }
    return 0;
}

static void dns_input(u32 src, u16 sport, const u8 *d, u32 len)
{
    (void)src;
    (void)sport;
    if (dq.done || len < DNS_HDR || get16(d) != dq.id)
        return;
    u16 flags = get16(d + 2);
    if (!(flags & 0x8000))
        return;                 /* not a response */
    if ((flags & RCODE_MASK) != 0) {
        dq.done = -1;
        return;
    }
    u16 qd = get16(d + 4), an = get16(d + 6);
    u32 off = DNS_HDR;
    for (u16 i = 0; i < qd; i++) {
        off = dns_name_skip(d, len, off);
        if (!off || off + 4 > len)
            return;
        off += 4;               /* QTYPE, QCLASS */
    }
    for (u16 i = 0; i < an; i++) {
        off = dns_name_skip(d, len, off);
        if (!off || off + 10 > len)
            return;
        u16 typ = get16(d + off), cls = get16(d + off + 2), rdlen = get16(d + off + 8);
        off += 10;
        if (off + rdlen > len)
            return;
        if (typ == QTYPE_A && cls == QCLASS_IN && rdlen == 4) {
            memcpy(&dq.ip, d + off, 4);
            dq.done = 1;
            return;
        }
        off += rdlen;
    }
    dq.done = -1;               /* answers, but no A */
}

int net_dns_lookup(u32 server, const char *name, u32 *ip)
{
    u8 q[300];
    if (!server || !name || !name[0] || strlen(name) > 253)
        return 0;
    memset(&dq, 0, sizeof dq);
    dq.id = (u16)(net_ms() ^ (u16)name[0] << 8 ^ (u16)strlen(name));
    if (!dq.id)
        dq.id = 1;
    dq.sport = (u16)(49152 + (net_ms() & 0x3FFF));
    put16(q, dq.id);
    put16(q + 2, 0x0100);       /* RD */
    put16(q + 4, 1);            /* QDCOUNT */
    put16(q + 6, 0);
    put16(q + 8, 0);
    put16(q + 10, 0);
    u32 n = dns_name_encode(q + DNS_HDR, sizeof q - DNS_HDR - 4, name);
    if (!n)
        return 0;
    put16(q + DNS_HDR + n, QTYPE_A);
    put16(q + DNS_HDR + n + 2, QCLASS_IN);
    n += DNS_HDR + 4;
    net_udp_bind(dq.sport, dns_input);
    int ret = 0;
    for (int tries = 0; tries < DNS_TRIES && !dq.done; tries++) {
        if (net_udp_send(server, dq.sport, DNS_PORT, q, n))
            break;
        u64 end = net_ms() + (500u << tries);
        while (net_ms() < end && !dq.done)
            net_poll(20);
    }
    net_udp_bind(0, NULL);
    if (dq.done == 1) {
        *ip = dq.ip;
        ret = 1;
    }
    return ret;
}
